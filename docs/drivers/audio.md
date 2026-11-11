# Audio: four FS19xx speakers on TDM, DMICs on the VA macro, through the ADSP's AudioReach: PianoAudio

Status, 2026-11-11 (night): `package/PianoAudio` levels 1-7 are built, host-tested and linked into the native KC, each inert below
its level and all of it inert without `piano-audio`; **nothing of it has run on hardware**. Level 1 is the read-only survey, level 2
the DSP link (one readiness query through PianoPower's client contract, from a GPR port set chosen by boot argument), level 3 the
macOS "Speaker" and "Mic" devices on a null transport (silence, no hardware). New tonight, on the host-tested pieces (§7.1): level 4
the LPASS clocks through the DSP's resource manager (PRM), level 5 the four amplifiers (bit-banged bus, the owner's preset staged on
the ESP, configured powered down and muted), level 6 the speaker graphs with a silent stream and the microphone graphs opened and
closed (the TDM pins, the SMMU bypass of the DSP's PCM streams, the buffers in PianoDspMemory.h's audio slice), level 7 the tone walk
(<= -20 dBFS, <= 3 s of tone, a 5 s watchdog) behind an explicit `piano-audio-tone=1`. The batched tablet test list: §8.8.
Template: `README.md`.

## 1. Purpose and gate

Speakers and microphones for native macOS. Gates no login milestone (plan §4(d).6: "Wi-Fi, Bluetooth, audio, sensors, later");
it is the last piece of a tablet that can replace Ubuntu for media and calls. Today the native boot runs `coreaudiod` and
`audiomxd` (11-11 07:26 run: pids 282, 292, the HAL's driver helpers) but the HAL finds no device.

The finding of this page: on piano the whole path is DSP-driven and the DSP is already up natively (PianoPower level 2), every
protocol piece has a hardware-verified BSD donor (UniXNU-sheng's GPR/AudioReach on SM8550; the VM-era audio kext for the macOS
device on this very macOS 27), and the piano-specific work is small but risky: the four FS19xx amplifiers (bus, reset, the vendor
preset), the TDM sink instead of sheng's I2S, a DMA buffer the DSP can address, and speaker safety without any speaker protection.

## 2. Hardware facts

Sources: DT = `out/inputs/piano-boot.dtb` (Ubuntu's running DT); UBU = `xiaomipad-8pro-mainline` `device/audio/` (README, m4, UCM,
`card.json`), `firmware/piano-audio.json`, `tools/speaker-playback-check.py`; INV = the local bring-up notes (`ubuntu-hardware-inventory.md`, not published)
§8; SHENG = UniXNU-sheng `porting.toml` area "audio" (SM8550, same DSP family, same hypervisor); RUN = `logs/native-runs/20261111-072646-loop`
(PianoPower level 2). All register values of level 1 are in `package/PianoAudio/AudioHw.h`.

| Item | Value | Source | State |
|---|---|---|---|
| Speakers | **four**, each with its own **FourSemi FS19xx** smart amplifier (`foursemi,fs19xx`), I2C 0x34 bottom left, 0x35 top left, 0x36 bottom right, 0x37 top right (landscape, camera up; bottom pair "confirm by ear" in UBU) | DT, UBU | verified (DT) |
| Amplifier bus | `i2c@980000`, the I2C hub's SE0 (`qcom,geni-i2c-master-hub`, wrapper `geniqup@9c0000`), 400 kHz, only these four devices; pins gpio64/gpio65 `i2chub0_se0` (their first function in the SoC's pin table), 2 mA, pull-up; the engine's clocks `se`/`core` (GCC) and `s-ahb` (the wrapper), interconnects `qup-core`/`qup-config` (RPMh votes) | DT, Linux pinctrl-sm8750 | verified (DT); state at hand-over: needs-boot (level 1); **decided: bit-banged** (§4) |
| Amplifier lines | reset (active low): 0x34 gpio128, 0x35 gpio129, 0x36 gpio107, 0x37 gpio108 (107/108 are eGPIO-capable: muxing them to a TLMM function claims them, as Linux's msm pinctrl does); interrupt (level low): gpio66, 84, 81, 67; no supply in the DT; Ubuntu holds reset 10 ms, releases it, then polls DEVID every 5 ms for up to 100 ms (the first power-up after a cold boot answers late) | DT, Linux fs19xx.c / pinctrl-msm.c | verified (DT); levels: needs-boot (level 1) |
| Amplifier identity | DEVID (reg 0x03) 0x2900 on all four; STATUS (0x00) bits BOVDS, PLLS, OTPDS, OVDS, UVDS, OCDS, CLKS, OTWDS, BOPS, BOLDS, INDS, VDDPS; gain 0.375 dB steps, 255 max | UBU | verified (source) |
| Amplifier preset | `foursemi/piano/fs19xx.fsm`, 4348 bytes, from the stock **odm_a** partition: per-amplifier register tables (TDM slot registers 0x1a/0x1c, 0xbd/0xbe, scene masks), CRC16; the same file on every unit of the build, no per-unit calibration (the stock driver reports re25 0) | `firmware/piano-audio.json` | verified (metadata; the pinned size and SHA-256 and the parser checked locally against the Ubuntu install's copy: stock, four blocks of 36 common and 140 scene units); read at run time from the owner's copy staged on the ESP (§10.3), never committed |
| Speaker link | LPAIF **secondary TDM** (`SECONDARY_TDM_RX_0` = q6apm-lpass-dais dai 40): 48 kHz, 4 x 32-bit slots, mask 0xf, bit clock 6.144 MHz from the SoC (internal sync), long sync, data delay 1; slot 0 TL, 1 TR, 2 BL, 3 BR; the DSP's TDM sink module 0x0700100E on LPAIF interface 1; bit clock = q6prm clock 0x25 (the DT's `bclk` of dai@28) | DT, UBU | verified (DT, source); the sync mode is UBU's one guess |
| TDM pins | main TLMM gpio121 `i2s1_sck`, 122 `i2s1_data0`, 123 `i2s1_ws`, 124 `i2s1_data1`, 8 mA (the sound node's pinctrl); i2s1 is each pin's first function (mux value 1) | DT, Linux pinctrl-sm8750 | verified (DT, source); mux at hand-over: needs-boot (level 1) |
| SoundWire / WSA / RX | WSA macro `0x6b00000` + SoundWire `0x6b10000`, WSA2 `0x6aa0000` + `0x6ab0000`, RX macro `0x6ac0000` + `0x6ad0000`, TX SoundWire `0x7630000`: all `disabled`. **No SoundWire and no WSA88xx on piano's speaker path** (Project-SunUEFI's early WSA884x guess was replaced by FS19xx on TDM) | DT, SUN | verified |
| Microphones | DMICs on the **VA macro** `codec@7660000` (okay); four DMIC pin pairs on LPASS LPI pins gpio6/7, 8/9, 12/13, 17/18, 4 mA; `qcom,dmic-sample-rate` 2.4 MHz (divider 4); capture `VA_CODEC_DMA_TX_0` (dai 110), 2 ch 48 kHz; UCM: DEC0 <- DMIC0 (side top), DEC1 <- DMIC2 (side bottom), gain 124 (+40 dB); DMIC4 top, DMIC6 back unused. TX macro `0x6ae0000` okay (Linux's aux device) | DT, UBU | verified (DT); four mics: inferred (stock ODM mixer overlay via UBU) |
| LPASS clocks and power | no LPASS clock controller for the application processor in the DT: every macro clock is a **q6prm** clock of the DSP (macro vote 0x66, dcodec vote 0x67, mclks 0x39 TX/VA, 0x40 RX, 0x42 WSA, 0x44 WSA2); the DSP's domains lcx/lmx; LPASS NoCs ag/lpiaon/lpicx. PRM's own IDs differ from the DT indices: **the VA macro's `mclk` is DT 57 = `LPASS_CLK_ID_TX_CORE_MCLK`, PRM 0x30c** (not the VA core mclk 0x307), set to 2 x 9.6 MHz = 19.2 MHz, attribute 1; `macro`/`dcodec` are the hardware-core votes 1 and 2, enabled macro, dcodec, mclk in that order; the TDM bit clock DT 37 = PRM `SEC_TDM_IBIT` 0x202, attribute 1, requested by the machine driver's `set_sysclk` before the graph is prepared | DT, Linux q6prm(-clocks).c, q6dsp-lpass-clocks.c, lpass-va-macro.c, q6apm-lpass-dais.c, sc8280xp.c | verified (DT, source); granted: needs-boot (level 4) |
| DSP services | `remoteproc@6800000` (PianoPower boots it); GLINK edge `lpass` (pid 2); `gpr` on channel `adsp_apps`, domain 2; services 1 q6apm (protection domain `avs/audio` in `msm/adsp/audio_pd`), 2 q6prm | DT | verified (DT) |
| Audio PD natively | with PianoPower level 2 the DSP **opened `adsp_apps`** (nobody serves it yet) next to glink_ssr, pcie_drv, mhi_sat, LOOPBACK_CTL_LPASS, fastrpcglink-apps-dsp; the locator answered tms/servreg, pddump, pdr; `msm/adsp/audio_pd` -> `avs/audio` is in PianoQrtr.c's domain table | RUN line 1173 | **verified (hardware)** |
| DMA | the q6apm dais' SMMU streams `0x1001` (mask 0x80) and `0x1041` (mask 0x20); SHENG: the DSP takes a **32-bit** address with the stream index (1) in bits 32 and up, the audio streams in SMMU pass-through through a bypass bank (0x1001, 0x1081, 0x1061 there; programming a *translating* context for them reset that board every time), buffers in low DRAM mapped write-combined. Piano's XNU window is 0x8c0000000 and up: no kernel allocation is DSP-addressable. **Decided**: PianoDspMemory.h's audio slice, 0x9da00000 + 16 MiB (the CDSP carveout, unused on piano), speaker periods at +0 and capture periods at +64 KiB; the four exact ids 0x1001/0x1081/0x1041/0x1061 through bypass banks (SmmuStream.h, as PianoSensors does for its streams) | DT, SHENG, `el1-hyp-notes.md`, `sensors.md` §5 | verified (DT) / inferred (SHENG); entries at hand-over: needs-boot (level 1); the DSP reaching the slice: needs-boot (level 6) |
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
| GENI I2C master (FIFO), or the bit-banged I2C of the gauges / keyboard cover | sheng `GeniI2c.{h,c}`; PianoPower / PianoKeyboard soft I2C | BSD | level 5: **bit-banged**, `BitI2c.h` copied from PianoKeyboard (as PianoBacklight drives its hub bus): the hub engine needs its GCC clocks and two RPMh interconnect votes that no native driver sets up, and PianoAudio touches neither GCC nor RPMh; software I2C needs only gpio64/65 (~80 kHz moves the 4348-byte preset in well under 2 s) |
| SMMU bypass of a stream (a bank with translation off, VA64, the stream's S2CR pointing at it; an exact entry the firmware left is kept or redirected) | `package/PianoDwc3/SmmuStream.h` (hardware-verified for the USB stream; PianoSensors and PianoWifi carry copies) | BSD (this repo) | level 6: copied as `SmmuStream.h` (namespace only), for 0x1001/0x1081/0x1041/0x1061 |
| FAT reader over a byte read callback | `package/PianoPower/FatVolume.{h,c}` (PianoPower's firmware, PianoSensors' pack) | BSD (this repo) | level 5: copied, the preset read from the ESP |
| macOS device: `IOAudio2Device` + `IOAudio2DeviceUserClient` shape (registry description, status page and ring memory types, selectors 0-12, hog mode, per-device period engine), UIDs "Speaker"/"Mic" | the VM-era audio kext (477 lines, its virtio-snd transport 382; provenance `package/PianoAudio/UPSTREAM`) | BSD-2-Clause-Patent | **level 3** (done, `PianoAudioDevice.cpp`, UPSTREAM; the native kext rule forbids linking a kext from outside this repository, not adopting its code as this repo's package); virtio-snd replaced by `AudioPcm.h` (null now, the DSP at level 8) |
| AudioServerPlugIn `ShengAudio.driver` + `ShengAudioService` (IOUserClient, shared rings, one timeline) | sheng `hal/ShengAudioPlugIn.c`, `ShengAudioService.cpp`, `ShengAudioAbi.h` | BSD | fallback of §5 |
| SM8750 GPR from XNU at EL1 under Gunyah: replies, WRITE_DONE 4100/4100, I2S on gpio121-124, 116 x 128-byte intents, GRAPH_OPEN 296 bytes | hackios-xnu-kernel-k90 `annibale_audio_reach.c` | APSL-2.0 | facts only (no sound confirmed there) |
| Module/parameter IDs, payload layouts (the TDM interface parameter) | linux-msm audioreach-topology (BSD-3-Clause; the Project-SunUEFI submodule is empty here), AudioReach engine headers (BSD-3-Clause-Clear) | BSD | the topology's ids are pinned in `PianoAudioReach.h`; the TDM parameter's layout from Ubuntu's kernel (specification) |

Nothing in scope implements FS19xx, a TDM sink or the VA macro under a BSD licence: those are written fresh from §3.

Taken over on 2026-11-11 (records in `package/PianoAudio/UPSTREAM`): sheng's GPR codec unchanged (`PianoGpr.{h,c}`), sheng's
AudioReach writer with piano's graphs (`PianoAudioReach.{h,c}`), the VM-era audio kext's IOAudio2 top half under this repository's names
(`PianoAudioDevice.cpp`); PianoPower's client interface is now the contract `package/include/PianoAdspContract.h` (charging.md §9).
For levels 4-7 (the same night): this repository's `BitI2c.h` (PianoKeyboard), `SmmuStream.h` (PianoDwc3) and `FatVolume.{h,c}`
(PianoPower) as byte-for-byte copies but for their provenance lines (the build script refuses a copy that drifted); sheng's
`AudioProbe.cpp` as design facts only (PRM requests from the PRM's own port; the audio streams through a bypass bank).

## 5. IOKit family / ABI: the macOS side

Level 1 is a plain `IOService` on `IOPlatformExpertDevice` (`com.apple.kpi.*` only). Later: what does the HAL talk to?

**What the KC offers.** The base vma2 KC (`kc27.bin`, 26A434) has `com.apple.iokit.IOAudioFamily` 800.1 (25 KB executable),
`IOAudio2Family` (a 2.5 KB shell, a dependency of IOAVFamily), `AppleUSBAudio` (needs IOAudioFamily) and `AppleVirtIO` (with
`AppleVirtIOSound`). The native KC keeps 48 kexts: only `IOAudio2Family` (with IOAVFamily) of these; `AppleAudioClockLibs` and
`AppleIISController` are ARMPlatformStandIn's stand-ins. No AppleHDA, no `IOUserAudio`/AudioDriverKit support; `IOUserServer` exists.

| Option | For | Against | Verdict |
|---|---|---|---|
| A. IOAudioFamily driver (`IOAudioDevice`/`IOAudioEngine`, KDK headers) | documented kernel API; AppleUSBAudio would come along | not in the native KC (the keep list is the k27 tree's); whether this build's HAL still drives IOAudio engines is unshown | not now (revisit for USB-C audio) |
| B. **`IOAudio2Device` top half** (the VM-era audio kext) under the stock HAL (`HALS_IOA2Manager` in audiomxd/coreaudiod, VirtualAudio's built-in Speaker/Mic classes) | **ran on this macOS 27** in the VM (sound, mic, 2 h soak), and natively it loaded and published Speaker/Mic (11-10/11 runs before the native kext rule); kernel only: no plugin bundle, no trust-cache entry, no HAL-directory overlay; host time is already 24 MHz natively (k27 0042), so none of sheng's host-clock patching | the user-client ABI is private (the ABI notes in `PianoAudioDevice.cpp`), tied to this OS build; VirtualAudio's speaker class loads Apple's speaker DSP chain of another product (EQ/limiter, not protection for these speakers) | **recommended** |
| C. AudioServerPlugIn (sheng's) + IOUserClient rings | public plugin API; hardware-verified on iPadOS 27 | natively: the bundle in the Data seed and its CDHash in the trust cache (the GPU plugin's staging), sheng's iOS workarounds (Thunderbolt transport type, the host-clock constant) to re-check on macOS; more parts | fallback if B misbehaves |
| D. AudioDriverKit dext | Apple's current model | needs a dext runtime, sysextd approval and entitlements; no IOUserAudio kernel side found | no |

Under B, PianoAudio publishes "Speaker" (stereo out) and "Mic" (mono or stereo in), 48 kHz S16, as the VM-era audio kext did. The kernel
maps stereo to the four 32-bit slots (L -> TL + BL, R -> TR + BR in landscape; rotation-aware mapping later), applies the volume
digitally, and owns every safety limit (§10.2) below whatever the HAL does.

**Built (level 3).** `PianoAudioDevice.cpp` is the VM-era audio kext's top half: `PianoAudioDevice : IOService` and `PianoAudioUserClient :
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
| `piano-audio=N` | 0 (probe() declines, nothing mapped) | 1: the read-only survey; 2: also the DSP link (§8.2); 3: also the macOS devices on the null transport (§8.3); 4: also the LPASS clocks (§8.4); 5: also the amplifiers, powered down (§8.5; without the DSP link level 5 runs alone); 6: also the graphs with silence (§8.6); 7: also the tone walk, only with `piano-audio-tone=1` (§8.7); >= 8: not built, runs 1-7 |
| `piano-audio-smmu=0` | 1 | skip the SMMU stream scan (global page only); level 6 then leaves the SMMU alone (stream 0x1001 must bypass already) |
| `piano-audio-ports=N` | 0 | the GPR source ports (§10.10, `AudioDspLink.h`): 0 Ubuntu's (APM 1, PRM 2, data 0x10000000), 1 sheng's (APM and data 0x2001, PRM 2), 2 SunUEFI's (APM 3, PRM 7, data 0x10000000) |
| `piano-audio-ports-try=1` | 0 | level 2's readiness query walks the other sets, each once, while nobody answers (10 unanswered queries a set); an answer of any kind keeps the set; every query is logged with its port |
| `piano-audio-lpass-read=0` | 1 | level 4 without the VA macro's and the LPI pins' register loads |
| `piano-audio-preset-any=1` | 0 | level 5 takes a valid preset container that is not the stock file (the safety filter still applies) |
| `piano-audio-i2c-swap=N` | 2 | the bus's SDA: 0 gpio64 (the DT's lane order), 1 gpio65, 2 gpio64 and, when no amplifier answers at all, gpio65 once |
| `piano-audio-i2c-half=US` | 5 | the software bus's half bit time (5: about 80 kHz) |
| `piano-audio-mic=0` | 1 | level 6 without the microphone graphs |
| `piano-audio-silence=MS` | 2000 | level 6's silent stream (at most 10000) |
| `piano-audio-tone=1` | 0 | **level 7 plays** (with `piano-audio=7`); anything else: level 7 is skipped and nothing is ever powered up |
| `piano-audio-tone-mask=M` | 0xf | the slots the walk visits (bit 0 TL, 1 TR, 2 BL, 3 BR) |
| `piano-audio-tone-ms=MS`, `piano-audio-tone-gap=MS` | 600, 150 | each slot's tone and the silence between; refused (level 7 skipped) above 3 s of tone in all or 4 s with the gaps |
| `piano-audio-tone-gain=G` | 160 | the amplifiers' gain for the walk, capped at 160 (-35.6 dB) whatever is given |
| `piano-audio-delay=S` | 0 | wait S seconds before the survey |
| `piano-audio-repeat=S`, `piano-audio-samples=N` | 0, 3 | resample the pins every S seconds, N times (one summary line each) |
| `piano-audio-dsp=0` | 1 | level >= 2 without the DSP link (e.g. to look at the macOS devices alone) |
| `piano-audio-dsp-wait=S` | 300 | give the DSP link up after S seconds without an answer |
| `piano-audio-settle=S` | 20 | the charger domain up this long (in its current generation) before audio's first packet |
| `piano-audio-device=0` | 1 | level >= 3 without the macOS devices |

The DSP link needs `piano-power=2` in the same boot (PianoPower starts the DSP; PianoAudio never starts or stops it): without it the
level logs "the DSP does not run in this boot" and registers nothing. The macOS devices do not need the DSP, nor does level 5 (it
then runs alone: `piano-audio=5 piano-audio-dsp=0`). Levels 4, 6 and 7 need level 2's "APM ready" in the same boot; level 6 needs
level 4 passed, level 7 needs levels 5 and 6 passed.

## 7. Build, kernel collection

`tooling/k27/build-native-piano-audio.sh`: the ten host tests (below; the preset loader on an mtools FAT32 image, skipped without
mtools), the copies checked against their originals (FatVolume, SmmuStream, BitI2c), the objects in `out/native-kexts/PianoAudio`
(17), the import check (392 kernel imports, 5 library imports: `piano_adsp_channel`, `piano_adsp_send`, `piano_adsp_health` from
PianoPower, `native_ufs_part_find`, `native_ufs_part_read` from NativeUfs; 0 unresolved) and `check-audio2-family.py` (§5).
Descriptor `tooling/k27/kext/PianoAudio.json` (`com.unixnu.driver.PianoAudio`), which names `com.unixnu.driver.PianoPower` and
`com.unixnu.driver.NativeUfs` among its libraries; listed in `configs/k27-native-kexts.txt` after both, so the KC resolves their
functions by name; `tooling/k27/build-native-image.sh` links it into every native KC (inert). Measured 2026-11-11 (night):
`PIANO_A830_METAL=0 tooling/k27/build-native-image.sh` links 21 native kexts, none from outside this repository
(`check-native-kexts.py`: the self-test, the descriptors, the KC log), PianoAudio 1233 pointers; `verify-ram-images.py` PASS.
`tooling/persist/build-esp.py --audio-preset FILE` stages the owner's preset (§10.3).

### 7.1 The code (host-tested; levels 1-7 run it on the tablet, level 8 and up wait for their level)

| Piece | Files | What it does | Host test (checks) |
|---|---|---|---|
| Level 1 survey | `AudioSurvey.h`, `AudioHw.h` | §8.1 | `audio_host.cpp` |
| GPR codec | `PianoGpr.{h,c}` (sheng's) | the 24-byte header, decode with its bounds, basic results; the APM readiness probe (one query in flight, retries, give-up) | `gpr_host.c` (37) |
| AudioReach packets | `PianoAudioReach.{h,c}` | GRAPH_OPEN of the four piano subgraphs (§3: device 0x4005 LOG 0x6050 -> MFC 0x6051 -> TDM 0x6052; stream 0x4001 WR_SH 0x6001 (out port 1) -> DEC -> CNV -> LOG 0x6004 -> 0x6050; mic 0x4008 codec DMA 0x6080 (VA, TX0) -> LOG 0x6081; capture 0x4003 LOG -> MFC -> CNV -> ENC -> RD_SH 0x6024); SET_CFG of the loggers (0x19ab/0x19af), the MFC (48 kHz, 32 bit, FL FR LS RS), the TDM sink (`PARAM_ID_TDM_INTF_CFG` 0x0800101B: LPAIF 0, interface 1, internal sync, data out, mask 0xf, 4 x 32, sync mode 1, delay 1; HW endpoint 48000/32/4; frame size factor 1), the decoder/converter, the WR_SH media format; PREPARE/START/STOP/CLOSE; MEM_MAP/UNMAP; WRITE/READ V2; PRM clock (SEC_TDM_IBIT 0x202 at 6.144 MHz) and votes; the DSP address (32-bit physical with the stream index above; XNU memory refused); the answer parser. Packet sizes 68-504 bytes (capture GRAPH_OPEN 504, stream 456, device 392): all within 512 | `audioreach_host.c` (276; GRAPH_START and the PRM clock request byte for byte) |
| Sequencer | `AudioSequence.{h,c}` | §10.1's rule as code: one command at a time, each answer checked (port, domain, token, opcode, status, handle), the first error unwinds what was done (and a step that may have half run) last first, without the DSP only the local undos; piano's speaker plan (clock, device graph, stream graph, map, prepare x2, start x2, amplifiers on; stop = its exact reverse, amplifiers first while the bit clock runs); level 4's PRM plans (the bit clock alone; the macro vote, the dcodec vote, the VA mclk 0x30c at 19.2 MHz); the microphone plan (the VA clocks, the mic device and capture stream graphs, the capture map, prepare x2, start x2; level 6 runs it to the prepares); the period pump (N periods in flight, error statuses, unknown slots, late periods) | `sequence_host.c` (4241: an error, a timeout and an abort at every step of every plan against a DSP model; every packet from its port in all three port sets) |
| DSP link | `AudioDspLink.{h,c}`, `AudioDsp.cpp` | the guard on PianoPower's health (§10.1), the receive inbox, level 2's exchange; the GPR port sets (Ubuntu's, sheng's, SunUEFI's) and the walk over them while nobody answers | `dsplink_host.c` (417) |
| Levels 4-7 | `AudioSession.{h,c}` | one state machine over the pieces: level 4's two PRM plans (the VA registers loaded while the votes hold), level 5 in one go, level 6 (pins, SMMU, buffers zeroed, the speaker plan with the amplifiers down, a silent stream, the status of the amplifiers every 100 ms: any not powered down stops it; the microphone plan to its prepares, undone), level 7 (the speaker plan with the amplifiers started muted at gain <= 160, unmuted after four silent periods came back, the walk through the safety chain into the DSP's buffer, the stream guard: 5 s watchdog, the group check every 100 ms, a refused or late period, quiet); PianoPower's health every tick: the guard off = no DSP command any more, the amplifiers muted, powered down and held at once; quiet = nothing new sent, a running stream stopped; at the end the amplifiers held in reset and the TDM pins given back | `session_host.c` (17343: a DSP model that plays one buffer per 10 ms and an amplifier model with reset lines in simulated time; every command of every level in order for the three port sets; at every amplifier write and every buffer: no power-up outside level 7's running graphs, no unmute before four silent periods, no unmuted gain above 160, no sample above -20 dBFS, only zeros while muted, at most 3 s of tone, the amplifiers down before a speaker graph stops; a refusal or a missing answer at the bit clock, the VA mclk, the stream start, the capture open, level 7's starts; a fatal report, a new link or charger generation, quiet, an amplifier's status change, a late or refused period, the watchdog, the owner's stop soft and hard, during the walk and during the silence; a missing amplifier, a preset that is not the stock file, a hostile preset, a damaged one, SDA/SCL swapped, a full queue) |
| Amplifier group | `AudioAmps.{h,c}` | the four in slot order: reset held 10 ms and released, probed; the preset's verdict (missing, too big, not the stock file unless `-preset-any`, a bad container); all four configured or none (the others held in reset again); start at min(gain, 160) muted, unmute, the group check against the first reading after the unmute, the way down (all four muted first, then each powered down), held in reset | `amps_host.c` (82) |
| Preset source | `AudioPreset.{h,c}`, `FatVolume.{h,c}` (PianoPower's) | `EFI/UniXNU/piano-fs19xx.fsm` read whole from the ESP (at most 64 KiB) | `preset_host.c` (22, on an mtools FAT32 image as `build-esp.py` makes the ESP) |
| Pins, SMMU, bus | `AudioPins.h`, `SmmuStream.h` (PianoDwc3's), `BitI2c.h` (PianoKeyboard's) | the TDM pins' i2s1 value, the reset lines' driven value (an eGPIO pin claimed), the bus lines' open-drain values, the LPASS loads of level 4; the bypass plan of the four PCM stream ids | `audio_host.cpp` (the values; the plan: free groups and banks, a bypassing entry kept, the DT's masked group refused without a write) |
| Kext glue | `AudioBoard.{h,cpp}`, `AudioDsp.cpp`, `PianoAudio.cpp` | the stores of AudioPins.h's values to the TLMM, the bit-banged bus, the SMMU writes through SmmuStream.h, the audio slice mapped write-combined, the VA/LPI loads, the ESP read through NativeUfs; the levels loop on level 2's channel, inbox and guard (2 ms ticks while a stream runs), bounded (an owner's stop after 180 s plus the silence, without DSP commands 30 s later) | KC link + import check |
| FS19xx | `Fs19xx.{h,c}`, `AudioSha256.{h,c}` | 8-bit register / 16-bit big-endian I/O through the owner's bus; probe (DEVID polled 100 ms, 0x29xx only); the `fs19xx.fsm` container (size, CRC16, per-address blocks, lists) without copying it; the units (whole, field, delay capped 100 ms, wait) through the safety filter (PWDN and the DAC mute kept, gain <= 192); configure (soft reset, coefficient, common table, music scene, shut down); start (48 kHz / 6.144 MHz only, the capped gain before the unmute, shut down again on a bus error); mute; shutdown (every write tried); status reads and the group check that stops a stream; the stock preset's published size and SHA-256 | `fs19xx_host.c` (287: a four-amplifier register model flags any write that powers up, unmutes or exceeds the cap outside start; a hostile synthetic preset) |
| Safety chain | `AudioSafety.{h,c}` | stereo/mono -> FL FR LS RS; DC blocker (-3 dB near 38 Hz); gain <= 0 dB; fade-in from silence at every start, fade-out to silence; clamp at a ceiling <= -1 dBFS (compile time), -20 dBFS by default; the 1 kHz test tone (<= -20 dBFS, <= 3 s, 20 ms ramps); level 7's walk (the tone in one slot at a time in slot order, gaps of zeros; <= 3 s of tone, <= 4 s in all); the stream guard (DSP error, late period, amplifier status, quiet, 5 s watchdog); the PCM ring | `pcm_host.c` (no random full-scale input crosses any ceiling; the walk sample by sample) |
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
| 4 (built) LPASS clocks/power | PRM through the sequencer from the port set's PRM port: the TDM bit clock 6.144 MHz (`SEC_TDM_IBIT` 0x202, attribute 1) requested and released; then the macro and dcodec votes and the VA mclk (TX core mclk 0x30c, 19.2 MHz), the VA macro's CORE_ID and the LPI DMIC pins loaded while they hold, all released in reverse (§8.4) | PRM answers 0 for every request and release; CORE_ID with the VA macro bit; `level 4: passed` | done (host); 1-2 boots |
| 5 (built) amplifiers | the bit-banged bus on gpio64/65 (SDA/SCL swapped once if nobody answers), resets held 10 ms and released, `fs19xx_probe` x 4; the owner's preset from the ESP (§10.3: the stock file only, `fsm_find` x 4), `fs19xx_configure` x 4, status read, all four held in reset again at the end (§8.5); runs without the DSP too | DEVID 0x2900 x 4, the preset "the stock file", container ok, "0 changed by the safety filter" or a plausible count, PWDN (PWRCTRL bit 0) set on all four | done (host); 1-2 boots |
| 6 (built) TDM link + graphs | TDM pins muxed (`sec-tdm-default-state`), the four PCM stream ids through bypass, the audio slice zeroed, the speaker plan with the amplifiers left down and 2 s of silence (their status every 100 ms), undone; the microphone plan to its prepares, undone; the pins given back (§8.6) | every command answers 0; the unwind answers 0 too; every period returned; the amplifiers' CLKS (and perhaps PLLS) bits appear while the device graph runs, PWDN stays set | done (host); 1-2 boots |
| 7 (built) test tone | only with `piano-audio-tone=1`: the speaker plan with the amplifiers started muted at gain 160, unmuted after 4 silent periods came back; the walk (1 kHz at -20 dBFS, 600 ms in each slot TL, TR, BL, BR, 150 ms apart: 2.4 s of tone) through the chain and the pump; the guard (5 s watchdog, the group check every 100 ms, a refused or late period, quiet, PianoPower's health); zeros, the amplifiers down, the plan undone, held in reset (§8.7) | WRITE_DONE for every period, STATUS equal on all four, audible on each speaker in turn (someone listens: the slot order by ear), `level 7: passed`, PWDN after | done (host); 1-2 boots, attended |
| 8 macOS device on the DSP | §5 B with the DSP transport: `audio_output_period` into the ring, the pump draining it, the plan on the transport's `running()` | `say` and `afplay` play | 1 week; 2-3 boots |
| 9 volume/mute | HAL volume -> `audio_chain_set_gain` (<= 0 dB) at fixed amplifier gain <= 192; mute = fade-out, zeros, then PWDN after 1 s; the ceiling raised from -20 dBFS (at most -1 dBFS) | volume keys work; no click at start/stop; the caps hold at maximum | 3-5 days; 1-2 boots |
| 10 microphones | PRM votes for the VA macro, LPI DMIC pins (DMIC0/DMIC2), VA macro DMIC clock divider 4, DEC0/DEC1, gain, high-pass; capture graphs (packets done: VA TX0 codec DMA, five-module stream); the first ~75 frames of a start transient dropped; the Mic device on the DSP transport | `tools/mic-capture-check.py` criteria (live, not clipped, >= -60 dBFS, non-flat) on a recording; Voice Memos | 1-2 weeks; 3-4 boots |
| 11 USB-C / Bluetooth | wired: USB host mode + a USB audio class path (option A's family) or the analog switch if a codec exists; Bluetooth with its own page | later | - |

Totals: levels 1-7 are built; about 3-5 weeks of agent work left (levels 8-10) and 12-20 owner boots to speakers, volume and
microphones in macOS, six to eight of them the batch of §8.8.

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
known from the VM-era audio kext: with a default output device WebKit's media player works.

### 8.4 Level 4: the LPASS clocks through PRM

After level 2's lines (now with the port set: `dsp: GPR ports: set 0 (Ubuntu: ...): APM from 0x1, PRM from 0x2, data from
0x10000000` and `APM_CMD_GET_SPF_STATE 1 sent from port 0x1 ...`). Every command has a `->` line (step, opcode, bytes, token, port)
and a `<-` line (done, or the status); these are the lines the host test's DSP model produces, the kext prints them with the
`PianoAudio: ` prefix:

```
PianoAudio: stage 7: levels 4 and up on the DSP link
PianoAudio: levels 4-4: ports Ubuntu (APM 0x1, PRM 0x2, data 0x10000000); the DSP link is up; preset the stock file only; tone off
PianoAudio: level 4: the LPASS clocks are the DSP's: requested and released through its resource manager (PRM, port 0x2)
PianoAudio: plan TDM bit clock: 1 steps, one at a time (APM from port 0x1, PRM from port 0x2)
PianoAudio: dsp: -> tdm bit clock 6.144 MHz (PRM REQUEST, 76 bytes, token 0x1000, from port 0x2)
PianoAudio: dsp: <- tdm bit clock 6.144 MHz: done in N ms
PianoAudio: level 4: the TDM bit clock is granted (6.144 MHz); releasing it
PianoAudio: dsp: -> undo: tdm bit clock 6.144 MHz (PRM RELEASE, 64 bytes, token 0x1100, from port 0x2)
PianoAudio: dsp: <- undo: tdm bit clock 6.144 MHz: done in N ms
PianoAudio: plan VA macro clocks: 3 steps, one at a time (APM from port 0x1, PRM from port 0x2)
PianoAudio: dsp: -> LPASS macro vote (PRM REQUEST, 60 bytes, token 0x2000, from port 0x2)          ... <- done
PianoAudio: dsp: -> LPASS digital codec vote (PRM REQUEST, 60 bytes, token 0x2001, from port 0x2)  ... <- done
PianoAudio: dsp: -> VA macro mclk 19.2 MHz (TX core mclk) (PRM REQUEST, 76 bytes, token 0x2002, from port 0x2)  ... <- done
PianoAudio: level 4: both votes and the VA mclk granted: the VA macro's and the LPI DMIC pins' registers (loads only)
PianoAudio: lpass: VA macro CORE_ID ........ ........ ........ ........: revision M.m step s, a VA macro
PianoAudio: lpass: LPI gpio6: CFG ........ VALUE ........: function F, pull P, N mA, output disabled, in V   (8 lines: 6 7 8 9 12 13 17 18)
PianoAudio: dsp: -> undo: VA macro mclk 19.2 MHz (TX core mclk) (PRM RELEASE, 64 bytes, token 0x2102, from port 0x2)  (then the two votes)
PianoAudio: level 4: passed: the bit clock, both votes and the VA mclk granted and released
PianoAudio: levels done in N ms: 8 packets sent, 8 received, 0 ignored, 0 unanswered, 0 times the queue was full
PianoAudio: levels: 4 passed; bus unused (0 transfers, 0 errors); preset not read; SMMU not set; DMA not mapped
PianoAudio: stage 8: levels done
```

Read from a failure: `<- tdm bit clock 6.144 MHz: status 0x..` (the PRM refused: no other DSP command follows in this boot; first
suspect the PRM port, then the attribute), `no answer to tdm bit clock ... within 2000 ms: undoing what was done, that step too` (the
release goes out anyway: nobody serves that PRM port; rerun with `piano-audio-ports=1`, sheng's PRM port is the same 2, or
`piano-audio-ports=2` for SunUEFI's 7), `CORE_ID 00000000 ...` or `NO VA macro bit` with both votes granted (the votes do not clock
the macro: level 10's problem, not level 6's). A hang right after the `lpass:` announcement means a VA or LPI load stalled: rerun
with `piano-audio-lpass-read=0` (the clocks alone).

### 8.5 Level 5: the amplifiers, powered down

Once: the owner's own stock preset on the ESP (the file of the tablet's odm_a, as the Ubuntu install carries it at
`/lib/firmware/foursemi/piano/fs19xx.fsm`, or a copy from the owner's backup; kept under the ignored `private/` or `out/`):

```sh
python3 tooling/persist/build-esp.py --profile daily --audio-preset private/audio/fs19xx.fsm   # plus the profile's usual options
# -> the ESP image with EFI/UniXNU/piano-fs19xx.fsm; refused unless it is the stock file (4348 bytes, sha256 ab060b84...);
#    written to the tablet by the maintenance session (persistent-boot.md), as the sensor pack is
```

A RAM-image boot reads it from the installed ESP. Without it level 5 still probes, then fails with `preset: none: no
EFI/UniXNU/piano-fs19xx.fsm on the ESP (tooling/persist/build-esp.py --audio-preset)` and holds the four in reset. Level 5 needs no
DSP, so its first run is without one:

```sh
PIANO_EXTRA_ARGS="piano-usb=2 piano-audio=5 piano-audio-dsp=0" PIANO_RESET_AFTER=300 tooling/k27/build-native-image.sh
```

Expected (the level 1 lines first; then):

```
PianoAudio: level 2: the DSP link skipped (piano-audio-dsp=0)
PianoAudio: stage 7: level 5 alone (no DSP link: levels 4, 6 and 7 need it)
PianoAudio: bus: gpio64 CTL ........ IO ........, gpio65 CTL ........ IO ........ as found: software I2C, the pins given back after each burst
PianoAudio: preset: EFI/UniXNU/piano-fs19xx.fsm: 4348 bytes, SHA-256 ab060b84..., the stock file
PianoAudio: levels 4-5: ports Ubuntu (...); no DSP link (level 5 only); preset the stock file only; tone off
PianoAudio: level 5: the amplifiers: resets held 10 ms and released, each probed, configured from the preset, left powered down and muted
PianoAudio: reset Top Left gpio129: CTL ........ IO ........ as found (held) -> CTL ........, driven          (4 lines, one per reset pin)
PianoAudio: bus: first acknowledge (0x35): SDA gpio64, SCL gpio65, software I2C at about 83 kHz
PianoAudio: amp Top Left 0x35 (slot 0): DEVID 0x2900 REVID 0x..          (x 4: Top Right 0x37, Bottom Left 0x34, Bottom Right 0x36)
PianoAudio: preset: 4348 bytes, the stock fs19xx.fsm (OS3.0.303.0, SHA-256 as Ubuntu publishes it); container ok; ok
PianoAudio: amp Top Left: configured: N writes, N reads, N changed by the safety filter, 0 unknown units, N waits missed, N OTP key writes   (x 4)
PianoAudio: amp Top Left: STATUS 0x.... (...) PWRCTRL 0x...1 SYSCTRL 0x0000 VOLCTRL 0x.... DACCTRL 0x.3.. : powered down   (x 4)
PianoAudio: the amplifiers muted, powered down and held in reset
PianoAudio: level 4: skipped: needs the DSP link (piano-power=2, level 2's APM ready)
PianoAudio: level 5: passed: four FS19xx answered, configured from the preset, powered down and muted
PianoAudio: levels: 4 skipped, 5 passed; bus software I2C (N transfers, N errors); preset EFI/UniXNU/piano-fs19xx.fsm: ...; SMMU not set; DMA not mapped
```

Read from it: the four DEVIDs (0x2900; any other high byte is refused), the counters of the safety filter (the stock preset's own
power, mute and gain units, each one kept powered down, muted, capped), `waits missed` (a preset wait whose field never matched while
powered down: counted, not an error), STATUS (compared across the four, not judged alone). `bus: nobody answered with SDA on gpio64:
now SDA gpio65` then answers: the lane order is the other way round (put `piano-audio-i2c-swap=1` into later runs). Nobody answering
either way: the reset lines (the `reset ...` lines' "as found" and the level-1 verdict), or the bus pull-ups (level 1's `bus ...
idle` word).

### 8.6 Level 6: the graphs with silence, the microphone graphs opened and closed

```sh
PIANO_EXTRA_ARGS="piano-usb=2 piano-power=2 piano-power-write=0 piano-audio=6" PIANO_RESET_AFTER=600 tooling/k27/build-native-image.sh
```

Expected after levels 4 and 5 (as above, now on the DSP link):

```
PianoAudio: level 6: the TDM pins, the DSP's PCM streams, the buffers in the audio slice, then the speaker plan with the amplifiers left down and a silent stream
PianoAudio: TDM: gpio121-124 to i2s1 (function 1, 8 mA, no bias) (CTL 000000c4 000000c4 000000c4 000000c4)
PianoAudio: SMMU stream 0x1001: free slot G (smr 00000000 s2cr 00000000) bank 81      (or: existing entry ..., already bypasses)
PianoAudio: SMMU stream 0x1001: through a bypass bank now                             (x 4: 0x1081, 0x1041, 0x1061)
PianoAudio: plan speaker (level 6: the amplifiers stay down): 15 steps, one at a time (APM from port 0x1, PRM from port 0x2)
PianoAudio: dsp: -> tdm bit clock 6.144 MHz (PRM REQUEST, 76 bytes, token 0x3000, from port 0x2)
PianoAudio: dsp: -> open speaker device graph (GRAPH_OPEN, 392 bytes, token 0x3001, from port 0x1)
PianoAudio: dsp: -> device logger (SET_CFG, 68 bytes ...)  device MFC (72)  device TDM interface (144)
PianoAudio: dsp: -> open playback stream graph (GRAPH_OPEN, 456 bytes, token 0x3005, from port 0x1)
PianoAudio: dsp: -> stream shared memory format (88)  stream decoder and converter (136)  stream logger (68)
PianoAudio: dsp: -> map the period buffer (MEM_MAP, 60 bytes, token 0x3009, from port 0x1)
PianoAudio: dsp: <- map the period buffer: done in N ms (map handle)
PianoAudio: dsp: -> prepare device, prepare stream, start device (bit clock runs), start stream (64 bytes each)   ... each <- done
PianoAudio: plan: amplifiers on: not at level 6 (they stay configured, muted and powered down)
PianoAudio: stream: silence (the amplifiers stay down): 10 ms periods of 3840 bytes, 8 slots, 6 in flight, WRITE from port 0x10000000, buffer 0x9da00000 (DSP address 0x19da00000), handle 0x..., watchdog 4000 ms
PianoAudio: amp Top Left: first STATUS 0x0040 (CLKS) PWRCTRL 0x0001 DIGSTAT 0x0000 at +N ms     (x 4; "now" lines when a bit changes)
PianoAudio: stream: done after 20xx ms: ~200 periods sent, the same returned, 0 late, 0 errors; 0 frames of tone, peak 0 (ceiling 3276): undoing the plan, the amplifiers first
PianoAudio: plan: amplifiers off: all four muted and powered down
PianoAudio: dsp: -> undo: start stream (GRAPH_STOP ...), start device (GRAPH_STOP), map (MEM_UNMAP, 44 bytes), the two GRAPH_CLOSEs, the bit clock (PRM RELEASE)
PianoAudio: level 6: the microphone graphs: the VA clocks, open, configure, map, prepare; then all undone (no capture yet)
PianoAudio: dsp: -> open mic device graph (GRAPH_OPEN, 344 bytes ...)  mic device VA TX0 codec DMA (152)  mic device logger (68)
PianoAudio: dsp: -> open capture stream graph (GRAPH_OPEN, 504 bytes ...)  capture MFC, converter and encoder (168)  capture logger (68)
PianoAudio: dsp: -> map the capture buffer (MEM_MAP, 60 bytes ...), prepare mic device, prepare capture
PianoAudio: level 6: the microphone graphs are open and prepared; closing them
PianoAudio: TDM: gpio121-124 given back as found (CTL ........ ...)
PianoAudio: level 6: passed: the speaker plan up and down, a silent stream, the mic graphs opened and closed
PianoAudio: levels: 4 passed, 5 passed, 6 passed; bus software I2C (...); preset ...; SMMU 0x1001 bypass, 0x1081 bypass, 0x1041 bypass, 0x1061 bypass; DMA zeroed and unmapped
```

Read from it: whether the DSP takes piano's graphs (a refused GRAPH_OPEN with its status: the first suspect is the module list or
the WR_SH port, §3), the map handle (a DSP that cannot reach the slice through the bypassed stream refuses the map or later returns
no period: `STOPPED: late period`), every period returned in time (the DSP renders the stream: the clocks run), and the amplifiers'
STATUS while the device graph runs: CLKS (bit 6) appearing is the TDM bit clock reaching them (sync mode and slot width right enough
for their clock detector; §10.11 if not). A refused SMMU plan (`the stream sits in a masked stream-match group`) means someone left
the DT's masked entry: level 6 fails with no SMMU write; `piano-audio-smmu=0` then tries with the entry as it is.

### 8.7 Level 7: the tone walk (attended: someone listens)

Charged battery, cable attached, the tablet flat on a table, someone listening within a metre; nothing plays without
`piano-audio-tone=1`:

```sh
PIANO_EXTRA_ARGS="piano-usb=2 piano-power=2 piano-power-write=0 piano-audio=7 piano-audio-tone=1" PIANO_RESET_AFTER=600 tooling/k27/build-native-image.sh
```

Expected after levels 4-6:

```
PianoAudio: level 7: the tone walk: the speaker plan with the amplifiers started muted at gain 160, unmuted after 4 silent periods
PianoAudio: plan speaker (level 7: the tone walk): 15 steps, ...                         (the same 14 commands as level 6, tokens 0x7000...)
PianoAudio: plan: amplifiers on: all four powered up MUTED, gain 160 (-35.6 dB), 48 kHz from the 6.144 MHz bit clock
PianoAudio: stream: the tone walk (silence first, the amplifiers muted): ..., watchdog 5000 ms
PianoAudio: amp Top Left: first STATUS 0x.... (PLLS CLKS ...) PWRCTRL 0x0000 DIGSTAT 0x.... at +N ms    (x 4)
PianoAudio: stream: 4 silent periods returned: the amplifiers unmuted; the walk starts (slots 0xf, 600 ms each, 150 ms apart, -20 dBFS)
           (heard: a 1 kHz tone from the top left, the top right, the bottom left, the bottom right speaker, in that order, 0.6 s each)
PianoAudio: stream: done after ~3000 ms: ~300 periods sent, the same returned, 0 late, 0 errors; 115200 frames of tone, peak 3276 (ceiling 3276): undoing the plan, the amplifiers first
PianoAudio: plan: amplifiers off: all four muted and powered down
PianoAudio: the amplifiers muted, powered down and held in reset
PianoAudio: level 7: passed: the walk played through; the amplifiers down; the plan undone
```

The listener notes, per tone, which speaker sounded (landscape, front camera on top). Pass: four tones, one speaker each, in the order
TL, TR, BL, BR, none distorted, nothing after the last one (no click at the end either). A different order is a slot mapping to fix
(the preset's slot registers or the MFC's map: note it; nothing is unsafe about it). Stopped early: the `stream: STOPPED: <reason>`
line names it (watchdog, DSP error, late period, amplifier status, quiet, or the guard's line before it) and the amplifiers were muted
and powered down at that moment. Only one speaker: `piano-audio-tone-mask=1` (TL alone) and so on for a second run.

### 8.8 The batched tablet test list (levels 1-7)

One boot each, in this order; read each run's log before the next (`tooling/native/usb-console.py`, or `grep PianoAudio` in the run's
`xnu-console.txt`), stop at the first failure and keep its log. RAM boots, cable attached, charged battery, owner present,
`PIANO_RESET_AFTER` as the backstop; `verify-ram-images.py out/ram/build-A-bundled.json` PASS before each. Every image is
`PIANO_EXTRA_ARGS="..." PIANO_RESET_AFTER=S tooling/k27/build-native-image.sh`.

| # | Arguments (`PIANO_EXTRA_ARGS`) | S | Attended | What it shows | Pass (expected lines) |
|---|---|---|---|---|---|
| 0 | (once, before 4) `build-esp.py ... --audio-preset <the owner's fs19xx.fsm>`, the maintenance session writes the ESP | - | owner | the preset where level 5 reads it | `build-esp.py`: accepted (the stock file); the JSON record's `audio_preset.stock` true |
| 1 | `piano-usb=2 piano-audio=1` | 300 | no | level 1 (§8.1): the hand-over state of the pins and SMMU | the survey lines; `verdict: ...` |
| 2 | `piano-usb=2 piano-power=2 piano-power-write=0 piano-audio=3` | 600 | no | levels 2-3 (§8.2-8.3) | `dsp: APM ready (status 1) after N queries from port 0x1 (set 0, Ubuntu)`; `devices: Speaker and Mic as IOAudio2Devices ...` |
| 2b | only if 2 gave `no answer within 300 s` with the channel up: `... piano-audio=2 piano-audio-ports-try=1` | 600 | no | which GPR port set the DSP serves (§10.10) | `nobody answered from port 0x1: GPR ports set 1 (sheng ...)` then `APM ready ... from port 0x2001 (set 1, sheng)` (or set 2, port 0x3); use that `piano-audio-ports=N` in every later run |
| 3 | `piano-usb=2 piano-power=2 piano-power-write=0 piano-audio=4` | 600 | no | level 4 (§8.4): the clocks are granted and released | `level 4: passed`; the CORE_ID line with "a VA macro"; PianoPower's lines unchanged (no `fatal`, link generation 0) |
| 4 | `piano-usb=2 piano-audio=5 piano-audio-dsp=0` | 300 | no | level 5 alone (§8.5): the bus, the four, the preset, no DSP | DEVID 0x2900 x 4; `level 5: passed`; `held in reset` |
| 5 | `piano-usb=2 piano-power=2 piano-power-write=0 piano-audio=6` | 600 | no (nothing is powered up) | level 6 (§8.6): the graphs, the bypass, the slice, silence | `level 6: passed`; every period returned; CLKS in the amplifiers' STATUS |
| 6 | `piano-usb=2 piano-power=2 piano-power-write=0 piano-audio=7 piano-audio-tone=1` | 600 | **yes: someone listens** | level 7 (§8.7): the walk | four tones TL, TR, BL, BR (by ear); `level 7: passed` |

Add `piano-audio-ports=N` from run 2b on when it chose a set, `piano-audio-i2c-swap=1` from run 4 on when level 5 swapped the lines.
Keep the other DSP clients and SMMU users out of these boots (no `piano-sensors>=2`, no `piano-wifi>=3`: §10.13). Six to eight
boots in all. Afterwards, with `piano-power-write=1` (PianoPower's normal writes) once, run 5 again: the graphs with the
charger configured as in daily use. Levels 8-10 (the macOS devices on the DSP, volume, the microphones) follow on these results.

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
   gain writes filtered, tests at 160 with tones <= -20 dBFS and <= 3 s (`audio_tone_init` and `audio_walk_init` refuse more;
   `audio_amps_start` caps the gain at 160 whatever is asked), only with `piano-audio-tone=1`, a 5 s watchdog
   (`audio_guard`), start muted (the plan powers the amplifiers last, `fs19xx_start(.., muted)`, the unmute after four silent
   periods came back, the chain fades in from silence),
   stop = fade-out, zeros, then the amplifiers shut down before the graphs stop (the plan's unwind order); any DSP error, late
   period or amplifier status change trips the guard; a DC blocker and a digital ceiling (-20 dBFS until level 9, never above
   -1 dBFS); no DSP volume module (the upstream one reaches +18 dB). The HAL's Apple speaker chain (option B) is not trusted as a
   limiter. Heat: no thermal driver; sessions attended.
3. **The amplifier preset.** `fs19xx.fsm` (TDM slot registers, per-speaker tables) is the stock file of odm_a: read at run time
   from the owner's own tablet, never committed, never disassembled. The parser (`fsm_check`/`fsm_find`) and the identity check
   (`audio_preset_is_stock`: 4348 bytes and the SHA-256 Ubuntu publishes for OS3.0.303.0.WPYCNXM) are done; a file that is not the
   stock one is refused unless the owner overrides it (`piano-audio-preset-any=1`). **Decided: the ESP.** odm_a is a logical partition
   in `super` (LP metadata) holding an Android EROFS image whose files may be LZ4-compressed: a reader for it is a new filesystem in
   the kernel for one 4348-byte file. Instead `build-esp.py --audio-preset FILE` puts the owner's own copy (the Ubuntu install's, or
   one from the owner's backup; checked there for the container, the size and the SHA-256) at `EFI/UniXNU/piano-fs19xx.fsm`, and level
   5 reads it through NativeUfs and a copy of PianoPower's FatVolume (`AudioPreset.h`), as PianoSensors reads its pack. Never committed,
   never in a published image; an odm_a reader (video, audio and sensors alike) can replace the staged copy later. The RAM-image
   carrier the sensors gained the same night (DarwinBoot's addon slot, `sensors.md` §5b, `SensorsAddon.h`) could carry the preset
   as a second entry, so that a RAM test needs no ESP write: a later change (one owner of the slot's container first). (level 5)
4. **A DSP-addressable buffer.** The DSP takes 32-bit addresses with the stream index above (`ar_dsp_address`; the plan refuses
   anything else); XNU's memory is above 4 GiB on piano. **Decided**: PianoDspMemory.h's audio slice (0x9da00000 + 16 MiB, the CDSP
   carveout's, `sensors.md` §5), of which the first 128 KiB are mapped write-combined at level 6 (speaker periods at +0, capture at
   +64 KiB), refused if it ever overlapped XNU's window; the four PCM stream ids through bypass banks (as sheng; a translating bank
   reset sheng's board). Open: whether the DSP's accesses through the bypassed stream reach the carveout (level 6's map and periods
   show it; the sensors' S2 shows the same for their slice). (level 6)
5. **PRM clock IDs and the TDM parameter.** The TDM bit clock is PRM's `SEC_TDM_IBIT` 0x202 (the DT's index 37 = 0x25), attribute 1,
   from Ubuntu's q6prm; **the VA macro's mclk is the DT's clock 57, `TX_CORE_MCLK`, PRM 0x30c** at 19.2 MHz (not sheng's VA core
   0x307; DT, specification), its macro and dcodec votes PRM's hardware cores 1 and 2. One attempt each with Ubuntu's values (sheng
   needed a second attribute on its SM8550 firmware for the TX mclk: if level 4 shows a refusal there, that is the first variant to
   try). The TDM parameter `PARAM_ID_TDM_INTF_CFG` 0x0800101B and its field order from Ubuntu's audioreach (specification only).
   (levels 4, 6)
6. **The amplifiers' bus: decided, bit-banged** on gpio64/65 (§4); which pin is SDA is the QUP lane order (gpio64 first, inferred):
   `piano-audio-i2c-swap=2` (the default) tries the other way once when no amplifier answers at all. The i2s1 function of gpio121-124
   is 1 (the SoC's pin table). Level 5 restores the bus pins after each burst; the reset lines stay driven low at the end (the
   amplifiers in hardware shutdown), whatever level 1 found. (level 5)
7. The DMIC supply (no regulator in the VA macro's DT node) and the 150 Hz high-pass / start transient (SunUEFI's notes). (level 10)
8. Option B's private ABI: pinned to this macOS 27 build; an OS update means re-checking the device's selectors, or option C.
9. USB-C wired audio: whether a WCD939x is fitted (stock symbols say maybe, the DT says no) and USB host mode first. (later)
10. **The GPR source ports.** Ubuntu's piano kernel sends APM commands from port 1, PRM requests from port 2 and data from dynamic
    ports (0x10000000 up); sheng (SM8550) sent APM commands and data from 0x2001 but PRM requests from 2, the PRM's own port (its
    first vote from 0x2001 was refused with status 1, `AudioProbe.cpp`), and the K90 (SM8750) used 0x2001 for the APM; Project-
    SunUEFI's patch says its piano ADSP answered only ports 3 (APM) and 7 (PRM) and dropped port 1. **Now a boot argument**:
    `piano-audio-ports=0|1|2` (Ubuntu's by default: the one stack that plays on this tablet), and `piano-audio-ports-try=1` lets level
    2's readiness query (a read-only question) walk the other sets while nobody answers; every query and every command is logged
    with the port it left from, and levels 4-7 use the set the link answered on. A PRM request that is processed but answered to a
    port nobody reads would leave a clock on: the sequencer releases a request that got no answer as if it had run. (level 2)
11. **The TDM sync mode.** Value 1 (long sync in the TDM API) is Ubuntu's guess between the stock ACDB's two entries (1 and 0);
    `ArTdm.sync_mode` is a field. First suspect if the amplifiers' CLKS/PLLS bits do not settle at level 6.
12. **Packet sizes.** The capture stream's GRAPH_OPEN is 504 of GPR's 512 bytes; any added module needs a second packet or a
   larger intent (the DSP's receive intents for `adsp_apps` are its own; the K90 saw 128-byte intents on its firmware, which
   PianoPower's GLINK would have to split or request). (levels 6, 10)
13. **Stream-match groups and banks have no common owner yet.** Level 6 takes free stream-match groups and bypass banks the way
   PianoSensors (level 2) and PianoWifi (level 3) do, each kext from its own scan; two of them planning in the same instant could
   pick the same group (`video.md` §4.2 asks for one registry with a lock word). Until it exists, the audio runs of §8.8 do not
   combine `piano-audio>=6` with `piano-sensors>=2` or `piano-wifi>=3`. (level 6)
14. **LPASS loads.** Level 4 loads the VA macro's CORE_ID and the LPI DMIC pins only after both votes and the mclk were granted (sheng:
   register access needs the two votes); a load from a block that is still unclocked may stall the bus. `piano-audio-lpass-read=0`
   is the way around it. (level 4)
15. **The start order and the gain.** `fs19xx_start` follows Ubuntu's order (power up, then the gain, then the unmute): for the
   few hundred microseconds between the first two the amplifier holds whatever gain the preset left, capped at 192 by the filter,
   but its DAC is muted then (the unmute comes only after four silent periods, at gain 160). The host tests check every unmute and
   every write while unmuted against 160. (level 7)
16. **Time spent shutting down.** The way down mutes all four first, then powers each down with Ubuntu's 35 ms: about 150 ms on
   the audio thread before the graphs stop. Every period queued meanwhile is zeros (the buffer is zeroed at a stop for trouble).
17. **The tone level.** -20 dBFS at gain 160 (-35.6 dB) is Ubuntu's unattended limit; nothing here can make it louder (the walk's
   peak, the chain's ceiling and the gain cap are compile-time limits). Heat: 2.4 s of tone, once per boot. (level 7)

## 11. Licence note

This repository's packages are BSD-2-Clause-Patent. Linux (q6apm, audioreach, gpr, q6prm and q6prm-clocks, q6dsp-lpass-clocks,
q6apm-lpass-dais, lpass-va-macro, the FS19xx driver, the piano machine driver sc8280xp.c, pinctrl-sm8750, pinctrl-msm, the LPASS
LPI pinctrl) and Project-SunUEFI's kernel patches are GPL-2.0: specification only (behaviour, IDs and register positions
as facts); no code, comment or table is copied. hackios-xnu-kernel-k90 is APSL-2.0: facts only. UniXNU-sheng and
the VM-era audio kext are BSD-2-Clause-Patent and are reused with `UPSTREAM` records; AudioReach's topology macros and
engine headers (BSD-3-Clause / BSD-3-Clause-Clear) supply IDs and layouts with attribution. No Qualcomm or FourSemi binary is
reverse engineered. The DSP firmware, the amplifier preset and any topology or ACDB file are data read at run time from the
owner's own tablet (or a staged copy under the ignored `out/`), never committed; the host tests use a synthetic preset container
with made-up register data and models of the DSP and the amplifiers, no recorded vendor data.
