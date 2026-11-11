# Sensors: ambient light, accelerometer, gyroscope, proximity through the ADSP's sensor hub: PianoSensors

Status: design + host-tested protocol code, 2026-11-11 (backlog B3 and B13). `package/PianoSensors` holds portable C only: the FastRPC
reverse file service, the QMI/SNS client codecs, the read-only file pack and its builder, a safety governor and the macOS-side policies,
all host-tested (`tests/run-host-tests.py`: ASan/UBSan, synthetic inputs, the kernel's no-FP/no-libc compile). **No kext, no boot
argument, nothing linked, nothing run on hardware.** Template: `README.md`.

## 1. Purpose and gate

Automatic brightness (ambient light), display rotation (accelerometer; the normal landscape while the keyboard cover is attached) and,
unused by macOS but reachable, gyroscope, proximity and magnetometer. Gates no login milestone; daily use. A second reason: the sensor
framework runs in the same DSP as the charger firmware, and on Ubuntu a sensor PD that could not get its files right crash-looped the
whole DSP (#3). Natively nothing serves those files today; nothing restarts a crashed DSP either, so a crash ends charging until reboot.

The finding of this page: everything is the DSP's. The application processor has three jobs on PianoPower's running DSP: (a) serve the
sensor PD's file reads over FastRPC (as hexagonrpcd does), (b) be a QMI client of the hub's sensor client service 400 (as libssc does),
(c) turn samples into a backlight target and a rotation. A BSD UniXNU component for another SM8750 device, PaganiSensors, already has (a)
and (b) as portable C (host-tested only there); piano adds the board facts, the safety rules and the macOS consumers.

## 2. Hardware facts

Sources: DT = Ubuntu's `sm8750.dtsi` + `sm8750-xiaomi-piano.dts` (kernel tree v1.1.0); FRPC = Linux `drivers/misc/fastrpc.c` of that tree;
IDL = qualcomm/fastrpc v1.0.7 (BSD-3, `inc/*.h`, `src/*_stub.c`, `src/apps_std_skel.c`, `src/listener_android.c`); HRPC = linux-msm/hexagonrpc
598b591a + the Ubuntu project's patches 0001-0007 (`device/sensors/hexagonrpc`); SSC = libssc 54dd13e3 protos and private headers; QMI =
libqmi's `qmi-service-ssc.json`; UBU = `xiaomipad-8pro-mainline` (`device/sensors`, `packaging/piano-sensors`, `firmware/sensors`); SUN =
Project-SunUEFI `docs/devel/hardware/sensors.md`; PAG = PaganiSensors 3d570b7; RUN = the native run of 2026-11-11 07:26 (PianoPower level 2).

| Item | Value | Source | State |
|---|---|---|---|
| Which DSP | the ADSP (LPASS, `remoteproc@6800000`): its carveout is `adspslpi` (the former sensor island is in the ADSP image); fastrpc nodes exist for `adsp` and `cdsp` only, no SLPI/SDSP; protection domain `msm/adsp/sensor_pd` (already in PianoQrtr.c's locator table) | DT, PianoQrtr.c | verified (DT) |
| FastRPC channel | GLINK `fastrpcglink-apps-dsp` on the `lpass` edge, label `adsp`, **secure** domain (no `qcom,non-secure-domain`: Linux's `/dev/fastrpc-adsp-secure`, the node Ubuntu uses) | DT, UBU udev rule | verified (DT) |
| Natively | with PianoPower level 2 the DSP **opens `fastrpcglink-apps-dsp`** (with glink_ssr, pcie_drv, mhi_sat, LOOPBACK_CTL_LPASS, adsp_apps, bt_cp_ctrl); nobody served it for 215 s and the DSP stayed up (slave-kernel 0x6: ready + handover, no fatal); the router listed 12 remote services (which: not logged) | RUN | **verified (hardware)** |
| Sessions | `compute-cb@3..8`, `reg` 3..8, `dma-coherent`; cb@3: SMMU streams `0x1003` mask 0x80 and `0x1043` mask 0x20 (cb@7: 0x1007/0x40, 0x1067, 0x1087). Linux hands out the first free session (sid 3). DMA mask 32 bits; the session id goes in address bits 32 and up (`sid_pos` 32 for `qcom,fastrpc`). `qcom,vmids` LPASS + ADSP_HEAP and the 12 MiB `adsp-rpc-remote-heap` serve static-PD creation (audio PD), not the sensors attach | DT, FRPC | verified (source) |
| Invoke wire | GLINK message of 40 bytes `{s32 client_id, s32 tid, u64 ctx, u32 handle, u32 sc, u64 addr, u64 size}`, answer `{u64 ctx, s32 retval}`; ctx = context id << 4 \| PD (sensors PD = 2); sc = attr<<29 \| method<<24 \| in<<16 \| out<<8 \| in-handles<<4 \| out-handles; addr/size = the payload: n `{u64 pv, u64 len}`, n `{u32 num, u32 pgidx}`, n `{u64 addr, u64 size}` (page lists, DMA address \| sid<<32), 16 u64 fds, 64 u32 crcs, then the inline buffers (128-byte aligned); attach to the sensors PD = handle 1 (`FASTRPC_INIT_HANDLE`), method 0, one in-buffer {client id}, message client id 0 (`FASTRPC_IOCTL_INIT_ATTACH_SNS`) | FRPC | verified (source) |
| Listener | remotectl = handle 0 (open 0, close 1: `{name len, err len}` + name -> `{handle, err}` + text); `adsp_default_listener` opened by name, method 0 registers; `adsp_listener` = handle 3: init2 3, next2 4 (inlines 256 bytes of the DSP's call, first call ctx 0 result -1), get_in_bufs2 5; HLOS side: `apps_remotectl` (handle 0), `apps_std`, `apps_mem`, `adspmsgd_apps`; the DSP's call is answered at the next next2 | IDL, HRPC | verified (source) |
| `apps_std` | methods 0 fopen, 3 fclose, 4 fread, 5 fwrite, 8 ftell, 9 fseek, 10 flen, 12 feof, 19 fopen_with_env, 20 fgets, 22 fileExists, 24 fremove, 26-28 opendir/closedir/readdir, 29 mkdir; 31 is the escape (the real id in in-buffer 0's first word): stat 31, ftrunc 32, frename 33 (sc `0x1f030000`); errors AEE 0xe EBADPARM, 0x14 EUNSUPPORTED | IDL, HRPC | verified (source) |
| What the PD reads | `/vendor/etc/sensors/sns_reg_config`, `/vendor/etc/sensors/config/*.json`, `/odm/etc/sensors/config/json.lst` + `*.json` (piano needs the ODM tree: HRPC 0003), `/mnt/vendor/persist/sensors/registry/{registry/, sns_reg_version, parsed_file_list.csv}` (also as `/persist/...`), `/sys/devices/soc0/{soc_id, revision, machine, family, platform_subtype(_id), platform_version, hw_platform}`. It **writes** each registry group as `registry/fstempfile` then frename into `registry/registry/` (about 110 per attach on piano) | HRPC 0003/0004/0007, UBU `sensors-prepare` | verified (source); the native list: needs-boot (S3) |
| SNS over QMI | service **400 (0x190)** on the DSP's router; request 0x20: TLV 0x01 u16 length + the protobuf (<= 1000 bytes), TLV 0x10 u8 report type (0 small, 1 jumbo); response: 0x02 result, 0x10 u64 client id, 0x11 u32; indications 0x21 (small) and 0x22 (jumbo, <= 10240) with 0x01 client id, 0x02 the protobuf. A response only says "received": data come in indications | QMI, PAG | verified (source); announced natively: needs-boot (S1) |
| SNS messages | request `{suid {low, high fixed64}, msg_id fixed32, susp_config {processor 1 = APSS, delivery 0 = wakeup}, request {batching, payload}}`; event `{suid, events {msg_id, timestamp (19.2 MHz), payload}}`; the SUID sensor is 0xabab... twice; ids 512/768 SUID request/event, 1/128 attributes, 513 stream at a rate, 514 on change, 10 disable, 1025 sensor event (floats + accuracy), 769 proximity (the SUID sensor's discovery-done), 130 error; data types `accel`, `gyro`, `mag`, `ambient_light`, `proximity`, `registry` (present once the PD has its registry: libssc's readiness probe) | SSC, PAG | verified (source) |
| Piano's sensors | ST **LSM6DSO** accel + gyro; QST **QMC6308** magnetometer; Sensortek **STK3BCX** front light/RGB/CCT + proximity; **SIP1328** rear light + flicker (`ambient_light_back`, `flicker`); Semtech SX937x SAR (RF, not ours); an OEM hall entry | UBU ODM config file names (contents not used), SUN runtime list | verified (names) / inferred (roles) |
| On Ubuntu | 2026-10-12: accel, gyro, light, proximity sampled (libssc `source.json`); SUN 10-08: accel (8.49, -0.08, 4.75) m/s^2 on the stand, compass 195 deg, light 153; GNOME's auto-brightness followed 138 -> 124 lux (backlight code 1256 -> 1135) | UBU, SUN | verified (their reports) |
| Axes | Ubuntu's mount matrix (x, y, z) = (y, -x, z) "docked +x -> normal"; with iio-sensor-proxy 3.9's rule (normal: y < 0, left-up: x > 0) the cover-docked pose (SSC +x up) is the normal landscape: `SensorsPolicy.h` PS_ACCEL_AXES; left/right never physically tested on Ubuntu either | UBU rules, iio-sensor-proxy | inferred; needs-boot (four-edge test, S4) |
| Calibration | persist's registry holds this unit's `fac_cal` (Ubuntu: unique per device, never public, never another unit's); the ROM's JSON carry generic defaults; **the DSP applies the calibration itself** from the registry it reads | UBU README | verified (source) |
| Crash signature | `fatal error received: err_qdi.c:1215:EF:qsh_process:0x4:UTIMER_CLIENT_4:0x1000002f:sns_watchdog.c:84:thread idx 1 exceeded processing time-limit`; a wedged DSP-written registry looped about 8 times a minute; afterwards the ADSP may fail with watchdog bites only | UBU `sensors-prepare`, device.md §2.14 | verified (source) |

## 3. Ubuntu as the specification

The chain: remoteproc boots the ADSP; udev on `fastrpc-adsp-secure` starts `piano-hexagonrpcd-sns`; `sensors-prepare` waits up to 5 s
for QRTR service 400, checks this boot's kernel log for >= 3 ADSP crashes with one sensor-watchdog fatal (then moves the registry state
aside, once per boot, keeping 3 copies), seeds the writable registry state from the owner's private copy or an empty default ("the DSP
generates the stock-default calibration"), and builds the file view (odm, vendor, `config`, `sns_reg.conf`, a socinfo copy, empty
`dsp/adsp` and `acdb`); `hexagonrpcd -f /dev/fastrpc-adsp-secure -d adsp -s -R view -w state` attaches to the sensors PD and serves it
(`Restart=no`: "a failed attach needs diagnosis/a normal reboot, not blind DSP re-attachment"). libssc (QMI over QRTR) polls the `registry`
SUID until the PD is ready, looks up each type's default sensor, keeps one QMI client per sensor, and enables with 513 (rate) or 514 (on
change), disables with 10. iio-sensor-proxy exposes only accel and light (proximity/compass deliberately off); GNOME rotates only in
tablet mode (cover detached) and runs gsd-power's ambient brightness.

Fixes that are design rules here: every listener request is answered, an unsupported one with an AEE error, never by vanishing (HRPC
0005: "the DSP-side sensor framework retries or continues on an error but aborts without the answer"); requests longer than 256 bytes
are completed with get_in_bufs2; enable/disable transitions are serialized, one request outstanding per client (sensorproxy 0004, 0007:
the v1.1.1 "light stops after off/on" fix: a nested close cancelled an open's first report); 0 lux is valid darkness (GSD
`ambient-zero.patch`); the DSP's first GLINK message is never dropped (K:235ab34e2a6e, PianoPower keeps it since 942af44).

## 4. UniXNU template to build on

**PaganiSensors** (UniXNU-Pagani-Drivers 3d570b7, BSD-2-Clause-Patent; OnePlus 13T, SM8750, iOS; host-tested only): taken unchanged
(sha256 in `UPSTREAM`): `SensorsPb` (protobuf), `SensorsSsc` (QMI + SNS), `SensorsRpc` (the reverse file service: attach, listener,
read-only `apps_std`, `apps_mem` heap pages, link loss), `SensorsPack` (the read-only file tree), `SensorsFixed`, `SensorsOrient`, and
their tests. Derived: the pack builder (piano's ODM archive, a soc0 allowlist, never written into a repository). Its manager
(`SensorsCore`: lookups, attributes, streams, retries) is the template of levels S2-S4, taken once PianoPower's router ports exist.
New: `SensorsGuard` (section 7), `SensorsPolicy` (section 5), `tests/piano.c`.

Cross-checks written here from the other sources, not from PAG (`tests/piano.c`): the SNS request bytes against libssc's messages, the
QMI frames against libqmi (also parsed by PianoPower's own `qcom_qmi_parse`), and the file service's whole bring-up against FRPC and the
IDL: attach `sc 0x00010000` with client id 0, open `0x00020200` "adsp_default_listener", register `0`, close `0x01010200`, init2
`0x03000000`, first next2 `0x04020200` {0, -1, 0, capacity}, the metadata layout and bank 3 in bits 32 and up. PAG's vector 4 is now framed
by PianoPower's QcomQrtr. All pass. Two deviations of PAG from Linux, both kept for now: ctx = id << 8 \| PD (Linux id << 4 \| PD; the DSP
echoes it, the PD sits in the low bits either way), and the buffers sit at fixed arena offsets with the message's `addr/size` covering only
the metadata page (Linux: one contiguous payload). The DSP maps each buffer by its page list; if S3 shows a refusal, the contiguous
layout is a contained change in `SensorsRpc.c`'s `invoke()`.

## 5. IOKit family / ABI: the architecture

```
PianoPower (owns the DSP: PAS, SMP2P, GLINK, IPCRTR, locator, servreg; never touched by sensors)
  |-- GLINK "fastrpcglink-apps-dsp" (existing client API) --> file service: attach PD 2, listener, apps_std from the pack (read only)
  |-- QRTR client ports (requested, 10.1 B) -------------> SNS client: service 400, a port per sensor
  '-- health: fatal bit, crash text, charger generation --> SensorsGuard (section 7) gates both
PianoSensors : IOService on IOPlatformExpertDevice (not HID: no macOS consumer reads sensor HID services for this display)
  |-- ambient_light (514, on change) -> SensorsPolicy curve -> PianoBacklight "PianoBacklightAmbient" (requested, 10.1)
  |-- accel (513, 10 Hz, only while the cover is detached) -> SensorsOrient -> property piano-sensors-edge
  |      -> launchd daemon piano-rotated: cover attached -> 0 deg; else the edge -> WindowServer rotation (+ PianoTouch's rotation)
  '-- proximity, gyro, mag: discovered and listed, never streamed
```

**The files.** One pack (`SensorsPack.h`) built on the host by `tools/make-sensor-pack.py` from archives of this tablet's own trees
(vendor and odm sensor directories, persist's `sensors/` mounted `ro,noload`, the allowlisted soc0 values), kept under the ignored
`out/` or `private/` and copied by the maintenance session to the ESP (partition 37, FAT32) as `\piano\sensors\pack.bin`, never into a
published image. The
kext reads it through NativeUfs's `native_ufs_part_find`/`native_ufs_part_read` and PianoPower's `FatVolume` (as PianoPower's firmware and
PianoWifi do) and **loads it whole into memory before the attach** (a few hundred KiB; refused above 4 MiB), so no DSP file call ever
waits for UFS. Writes (`fwrite`, `fremove`, `frename`, `ftrunc`) are declined with an error: persist stays pristine and nothing written by
the DSP can wedge the next boot (#3's cause). Ubuntu's V4 run sampled with an unchanged pristine registry. If S3 shows the PD retrying
writes in a loop, a volatile RAM overlay (fstempfile + frename, lost at reboot) is the next step. B13 at run time: a read-only ext4
reader for persist's `sensors/registry` (extents, linear directories, no journal replay) replacing the pack's copy (level S6).

**DSP-addressable memory.** The invoke payloads, the inline buffers and `apps_mem`'s heap pages must be reachable by the DSP through
stream 0x1003 with 32-bit addresses. In SMMU pass-through that means physical memory below 4 GiB, outside XNU's window (0x8c0000000 and
up): the same question as `audio.md` §10.4 (candidates: the UEFI's former "Kernel" region at 0xA8000000 or its DXE heap; PAG used 8 MiB
of conventional low memory). The alternative is a translating context bank with our own page tables (UniXNU's ArmSmmu4K): on piano the
HLOS programs the compute-cb banks (Linux does), unlike the audio streams sheng found to be the hypervisor's. 2 MiB of calls plus up to
6 MiB of heap pages (PAG's split); mapped write-combined, published with a barrier before each send.

**macOS consumers.**
- Ambient light: the curve (`ps_als_curve`: 0 lux 4 %, 100 lux 24 %, 300 lux 37 %, 1000 lux 57 %, 10000 lux 100 %, battery first),
  smoothing (1 s towards brighter, 4 s towards darker), 3 % hysteresis, at most one target per 250 ms; PianoBacklight applies it with
  the user's offset (the brightness keys shift the curve), its ramps and its caps. The stream stops while `piano-display-power` is off.
  macOS's own CoreBrightness needs a built-in panel with Apple's backlight service; NativeGopDisplay's panel is external (backlight.md §5).
- Rotation: the kernel publishes the edge that is up (`PS_EDGE_*`, after SensorsOrient's 250 ms hold and a 600 ms settle); a launchd
  daemon applies it, 1 s apart at most, and forces 0 deg while PianoKeyboard reports the cover attached (Ubuntu's rule). R1: WindowServer's
  own rotation (System Settings offers Rotation for external displays on Apple silicon; the daemon makes the same private call, pinned to
  this build); R2: NativeGopDisplay rotates in its copy and offers portrait modes (its owner); R3: none. Touch must follow (PianoTouch).
- Proximity: unused (no ear detection on a Mac; the screen goes off by lid/hall and display sleep). Gyroscope and magnetometer: unused;
  the gyroscope may later gate rotation while the tablet turns.

## 6. Boot args

| Argument | Default | Values |
|---|---|---|
| `piano-sensors=N` | 0 | 0 nothing; 1 read-only survey (no DSP traffic); 2 + SNS lookups/attributes; 3 + the file service; 4 + streams and the consumers |
| `piano-sensors-axes=a..` | `a+y+x+z` | the accelerometer map (SensorsFixed text), for the four-edge test |
| `piano-sensors-bank=N` / `-rpc-pa=ADDR` | 3 / none | the context bank (3..8); the arena's physical address (S3 refuses to run without one) |
| `piano-sensors-settle=S` | 20 | seconds of a healthy charger domain before the first request |
| `piano-sensors-als=0`, `-rotate=0`, `-log=0..2` | 1, 1, 1 | consumers off; log volume |

## 7. Kernel component, and the safety story

Component `PianoSensors` (`com.unixnu.driver.PianoSensors`), listed after PianoPower in `configs/k27-native-kexts.txt` once level 1
exists, inert without `piano-sensors`, imports PianoPower's client functions by name. Rules (`SensorsGuard.c`, host-tested):

1. A client only: never start, stop or restart the DSP, never vote its rails or touch RPMh/AOSS; no re-attach after a failed attach in
   the same boot (Ubuntu's `Restart=no`).
2. Charger first: nothing is sent until the DSP is up, `charger_pd` is up and its newest generation has been up for 20 s, and while
   PianoPower asks for quiet.
3. One strike: the SMP2P fatal bit, any SMEM crash reason, a new charger generation, the charger domain down, or a new GLINK link
   generation turns the sensors off for the rest of the boot.
4. Rate: 8 requests at once, 4 per second; a failure backs off 1, 2, 4 ... 64 s; 6 failures in 10 minutes: off.
5. The DSP's file calls are answered at once from memory; when off, each is still answered, with an error (a silent listener blocks a
   PD thread, which `sns_watchdog` kills the DSP for); more than 3000 file calls in a minute (a PD in a loop): off.
6. Read only: persist and the pack are never written; no per-unit value is logged at default levels; only the allowlisted soc0 names
   (no serial number) are served.

## 8. Hardware test plan (after PianoPower level 2 is healthy; attended, RAM boot, charged battery, cable attached)

| Level | What runs | Pass | Effort |
|---|---|---|---|
| S0 | (done offline) design, codecs, guard, policy, pack tool | `run-host-tests.py` PASS | done |
| S1 survey | `=1`: the router's services (0x190 there? instance), the `open:` list, SMMU entries of 0x1003..0x1008/0x1043.. (loads only), the pack found and valid on the ESP, the arena candidate's map type; no message to the DSP | all listed; nothing written; charger unchanged | 1-2 days, 1 boot |
| S2 SNS | `=2`: SUID lookups (`registry`, accel, gyro, ambient_light, proximity, mag) and attributes, no file service, no stream | answers parse; expected without files: `registry` maybe, no accel (SUN saw that with incomplete files); no fatal for 10 min, `charger_pd` generation and charge current unchanged | PianoPower ports (owner) + 2-3 days, 1-2 boots |
| S3 files | `=3`: attach (bank 3, then 4), listener, the pack served, writes declined, every path logged | attach retval 0; `sns_reg_config`, `json.lst`, the registry read; S2's lookups now find accel/gyro/light; no fatal for 15 min, charger unchanged | 3-5 days, 2-4 boots |
| S4 streams | `=4`: light on change -> PianoBacklight; accel 10 Hz -> the edge; 20 light off/on cycles | lux follows a lamp, the backlight within 2 s; readings after every cycle (v1.1.1); the four edges named right (fix `-axes` if not) | 3-4 days, 2 boots |
| S5 rotation | the daemon (staged and signed like the GPU plugin), the cover lock, touch rotation | each edge upright with touch at the four corners (Ubuntu's table); attached -> normal | 3-5 days, 2-3 boots |
| S6 B13 | persist's registry read at run time (ext4 reader), or the RAM overlay if S3 asked for it | readings identical with the pack's registry; at rest 9.81 +- 0.1 m/s^2 on each face; light within 10 % of Ubuntu's under one lamp | 3-5 days, 1-2 boots |

About 3-4 weeks of work and 9-14 attended boots. Fails safe: everything stops at the first guard event; a reboot returns to Ubuntu.

## 9. Fallback

`piano-sensors=0` (the default): nothing runs, as today. Brightness by the keys (PianoBacklight), rotation manual (R1's System Settings
entry, if it exists for this display) or none. Ubuntu keeps its own sensor stack.

## 10. Open questions and risks

### 10.1 The API asked of PianoPower (its owner adds it; PianoSensors does not edit PianoPower) and of the other drivers

- A. Existing, used as is: `piano_adsp_channel("fastrpcglink-apps-dsp", 512, 8, 0 /* the DSP opens it */, ...)` and `piano_adsp_send`
  (40-byte invokes; answers of 12-16 bytes). Asked: `kClients` from 4 to 6 (IPCRTR, PMIC_RTR_ADSP_APPS, audio's adsp_apps and this one
  would take all four).
- B. New, QRTR client ports on PianoPower's router (PaganiAdsp's host-tested interface, renamed): `piano_qrtr_open(service, instance, client,
  context)` with `server(up, node, port, instance)` and `receive(node, port, data, length)` callbacks on the DSP thread;
  `piano_qrtr_server(port, &server)` (node, port, instance, a generation that moves on every change); `piano_qrtr_send(port, node,
  to_port, data, length)` (queued, never waits); `piano_qrtr_close` (DEL_CLIENT). Up to 8 ports for sensors (`QCOM_QRTR_MAX_LOCAL` is 4,
  2 used). IPCRTR receive intents of 2048 bytes (today 1024; an attribute report can reach 1000 bytes of protobuf plus 50 of headers).
- C. Health, read only: `piano_adsp_health(&h)` = {SMP2P fatal seen, a dedicated crash-reason string (today it shares `text` with the
  `open:` notes), GLINK link generation}; `piano_qrtr_charger_pd(&ups)` exists. And `piano_adsp_quiet()`: nonzero while PianoPower
  wants clients still (charger configuration, its own recovery), polled by the guard.
- D. Not PianoPower's, shared with audio: one owner and one fixed range for DSP-addressable low memory (or a translating bank for
  0x1003/0x1043; PianoWifi's and PianoDwc3's `SmmuStream.h` shows the bypass form).
- PianoBacklight: `PianoBacklightAmbient` (param1 = target permille, or -1 to leave automatic mode; param2 = source), the user's offset
  kept across steps, `piano-backlight-auto=1`. PianoKeyboard: publish `piano-keyboard-attached` (0/1) in `coverAttached`. PianoTouch: a
  rotation input (0/90/180/270) applied to its absolute coordinates. NativeGopDisplay (R2 only): a transform in its copy and portrait modes.

### 10.2 Questions

1. Does the sensor PD stay quiet for hours with nobody attached (215 s seen)? (S1, a long charging run)
2. Which bank the DSP accepts for PD 2 (3 is Linux's first session on piano, 4 the 13T's) and whether 0x1003 is in bypass, fault or
   translation when the boot hands over. (S1, S3)
3. The arena's location (10.1 D). (S1 + owner decision with audio)
4. Whether the PD asks for `apps_mem` pages, socinfo, DSP libraries (`/usr/lib/qcom/adsp`, empty on Ubuntu) or files outside the pack. (S3 log)
5. Whether declined writes are harmless (Ubuntu's V4 says yes) or the PD loops (then the RAM overlay). (S3)
6. One QMI client per sensor (libssc, Android) or one for all: ports needed. (S2)
7. The rotation path: does System Settings offer Rotation for NativeGopDisplay's panel, and does a rotated desktop come through its CPU
   copy unchanged (WindowServer composites rotated) or as swaps with a transform? (S5)
8. The physical axes (left/right) and macOS's sense of "90 deg" (S4, S5: Ubuntu's four-edge table).
9. The front sensor's placement (bezel or under the panel: an under-display sensor would see the screen's own light). (S4, lamp off/on)
10. Jumbo reports: requested as "small" (TLV 0x10 = 0) to fit the intents; whether the hub then splits or drops large ones. (S2)

## 11. Licence note

PianoSensors is BSD-2-Clause-Patent. PaganiSensors (BSD-2-Clause-Patent) is reused with `UPSTREAM` records. Linux's fastrpc driver and
the SM8750 devicetree (GPL-2.0), hexagonrpc and the Ubuntu project's patches to it (LGPL-3.0 / GPL-3.0), libssc (GPL-3.0), iio-sensor-proxy
(GPL-3.0) and libqmi (LGPL-2.1) are specification only: behaviour, numbers and layouts as facts, no code or text copied. qualcomm/fastrpc
(BSD-3-Clause) was read for its IDL's numbers only. No Qualcomm, ST, Sensortek or Xiaomi binary is reverse engineered. The DSP firmware,
the vendor/ODM sensor configuration and this unit's persist registry are data read at run time from the owner's own tablet (or a pack
under the ignored `out/`), never committed and never another unit's.
