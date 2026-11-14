# Sensors: ambient light, accelerometer, gyroscope, proximity through the ADSP's sensor hub: PianoSensors

Status: levels 1-4 built, 2026-11-11 (backlog B3 and B13). `package/PianoSensors` is a k27 native kext on PianoPower's client
contract (`package/include/PianoAdspContract.h`, `charging.md` §9): level 1 a read-only survey, level 2 the FastRPC reverse file service
serving the owner's sensor pack from the ESP (or from a RAM test image that carries it, §5b), level 3 the SNS client's sensor list,
level 4 the streams: the ambient light on change and the accelerometer at 10 Hz, each on a QMI client of its own and only while a
consumer needs it, published as the properties §5 reserved (§5c). Host-tested (`tests/run-host-tests.py`: ASan/UBSan, synthetic inputs
only, a simulated DSP and hub, the pack on an mtools FAT32 image and in a RAM image's addon, the kernel's no-FP/no-libc compile), compiled
with the kernel's flags and **linked into the KC** (`configs/k27-native-kexts.txt`, after PianoPower and NativeUfs; the 2026-11-11
link with level 4: PianoSensors 461 pointers among 21 native kexts, the native kext rule passed), inert without
`piano-sensors` (and level 4's parts below `piano-sensors=4`). **On the tablet (2026-11-14):** S1 passed; S2's adsp_listener init2 was
answered EBADPARM and the sensor PD's watchdog then took the ADSP (§8.2); the framing fix and rule 7 are host-tested, the s1b probe
is built, not yet run. Template: `README.md`. The macOS-side consumers
(the rotation agent `piano-rotated`, PianoTouch's transform, PianoBacklight's automatic mode) and the streams' off/on lifecycle, which the
kext now runs itself, are `package/PianoAuto`, host-tested (§5a).

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
| Invoke wire | GLINK message of 40 bytes `{s32 client_id, s32 tid, u64 ctx, u32 handle, u32 sc, u64 addr, u64 size}`, answer `{u64 ctx, s32 retval}`; ctx = context id << 4 \| PD (sensors PD = 2); sc = attr<<29 \| method<<24 \| in<<16 \| out<<8 \| in-handles<<4 \| out-handles; addr/size = the payload: n `{u64 pv, u64 len}`, n `{u32 num, u32 pgidx}`, n `{u64 addr, u64 size}` (page lists, DMA address \| sid<<32), 16 u64 fds, 64 u32 crcs, then the inline buffers (128-byte aligned); **every invoke carries that payload page, also one without buffers** (`fastrpc_internal_invoke` always runs `fastrpc_get_args`: 384 zero bytes, size 4096, the sid above bit 32); the message's tid is the calling task's id, not the client id; attach to the sensors PD = handle 1 (`FASTRPC_INIT_HANDLE`), method 0, one in-buffer {client id}, message client id 0 (`FASTRPC_IOCTL_INIT_ATTACH_SNS`); release = handle 1, method 1 (`FASTRPC_RMID_INIT_RELEASE`), the same in-buffer, client id 0, sent when the daemon's file closes | FRPC | verified (source) |
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
their tests. Derived: the pack builder (piano's ODM archive, a soc0 allowlist, never written into a repository) and `SensorsRpc` (an
`admit` hook: a call the guard declines is answered at once with `SSR_EUNSUPPORTED`, counted as `declined`). PAG's manager
(`SensorsCore`: streams, retries, iOS units) was the template of levels 3 and 4, not taken. New: `SensorsGuard` and `SensorsHealth.h`
(section 7), `SensorsPolicy` (section 5), `SensorsClient` (level 3: one connection, lookups then attributes, the guard asked for each
request), `SensorsStream` (level 4, §5c: a client per stream over PianoAuto's `AutoStream`/`AutoLight`), `SensorsLoad` and
`SensorsAddon` (the pack from the ESP or a RAM test image), `PianoSensors.cpp` (the kext), `tests/piano.c`, `tests/streams.c`,
`tests/load.c` and `tests/addon.c`.

Cross-checks written here from the other sources, not from PAG (`tests/piano.c`): the SNS request bytes against libssc's messages, the
QMI frames against libqmi (also parsed by PianoPower's own `qcom_qmi_parse`), and the file service's whole bring-up against FRPC and the
IDL: attach `sc 0x00010000` with client id 0, open `0x00020200` "adsp_default_listener", register `0`, close `0x01010200`, init2
`0x03000000`, first next2 `0x04020200` {0, -1, 0, capacity}, the metadata layout and bank 3 in bits 32 and up. PAG's vector 4 is now framed
by PianoPower's QcomQrtr. All passed, and missed one fact: both simulated DSPs (PAG's and ours) required `addr 0 size 0` for an invoke
without buffers, which is PAG's framing, not Linux's (§2: Linux sends the payload page with every invoke). The tablet's first S2 answered
exactly those two invokes differently: register took it, adsp_listener's init2 answered EBADPARM (§8.2). Since 2026-11-14 the framing is a
session property (`SSR_FRAME_*`, `piano-sensors-rpc-frame`): **meta** (the default) sends Linux's page with every invoke; an init2 answered
with an error is tried again in the same session with **ctx4** added (ctx = id << 4 \| PD as Linux's tree; PAG: id << 8 \| PD, which leaves
bits 4..7 clear) and then **tid** (a thread id of its own, as Linux's task id; PAG: the client id); the framing that is answered 0 serves the
session. The simulated DSPs now hold the page to Linux's fact (`tests/bringup.c` scripts the three framings). One deviation stays: the
buffers sit at fixed arena offsets with the message's `addr/size` covering only the metadata page (Linux: one contiguous payload). The DSP
maps each buffer by its page list (remotectl open's name and handle went through it on the tablet); a refusal of that would be a
contained change in `SensorsRpc.c`'s `invoke()`.

## 5. IOKit family / ABI: the architecture

```
PianoPower (owns the DSP: PAS, SMP2P, GLINK, IPCRTR, locator, servreg; never touched by sensors)
  |-- GLINK "fastrpcglink-apps-dsp" (contract) ----------> file service: attach PD 2, listener, apps_std from the pack (read only)
  |-- a QRTR client port following service 400 ----------> SNS client: lookups, attributes (level 3)
  |-- one more port per stream (level 4) ----------------> the light's and the accelerometer's own QMI clients (§5c)
  '-- piano_adsp_health / piano_adsp_quiet --------------> SensorsGuard (section 7) gates all of it
PianoSensors : IOService on IOPlatformExpertDevice (not HID: no macOS consumer reads sensor HID services for this display)
  |-- ambient_light (514, on change; AutoLight) -> SensorsPolicy curve -> piano-sensors-light-target -> PianoBacklight (§5a.4)
  |-- accel (513, 10 Hz, only while the cover is detached; AutoStream) -> SensorsOrient -> property piano-sensors-edge (§5a.5)
  |      -> launch agent piano-rotated: cover attached -> 0 deg; else the edge -> WindowServer's rotation -> PianoTouch (§5a.2, 5a.3)
  '-- proximity, gyro, mag: discovered and listed, never streamed
```

**The files.** One pack (`SensorsPack.h`) built on the host by `tools/make-sensor-pack.py` from this tablet's own registry backup and
the stock ROM's sensor configuration (section 5b), kept under the ignored `out/` or `private/` and put on the ESP (partition 37, FAT32, 4096-byte sectors) as `EFI/UniXNU/piano-sensors.pack` by
`build-esp.py --sensor-pack` (section 5b), never into a published image. The kext (`SensorsLoad.c`) finds partition `esp` through
NativeUfs's `native_ufs_part_find`/`native_ufs_part_read`, reads it with its copy of PianoPower's `FatVolume` (as PianoPower's firmware
and PianoWifi do) and **loads the pack whole into memory and checks it before the attach** (a few hundred KiB; refused above 4 MiB), so
no DSP file call ever waits for UFS. Level 2 does not start without a valid pack (`piano-sensors-nopack=1` serves an empty one: every
open fails and is logged, the way a first run learns what the pack must hold). Writes (`fwrite`, `fremove`, `frename`, `ftrunc`) are
declined with an error: persist stays pristine and nothing written by the DSP can wedge the next boot (#3's cause). Ubuntu's V4 run sampled with an unchanged pristine registry. If S3 shows the PD retrying
writes in a loop, a volatile RAM overlay (fstempfile + frename, lost at reboot) is the next step. B13 at run time: a read-only ext4
reader for persist's `sensors/registry` (extents, linear directories, no journal replay) replacing the pack's copy (level S6).

**DSP-addressable memory.** The invoke payloads, the inline buffers and `apps_mem`'s heap pages must be reachable by the DSP through
stream 0x1003 with 32-bit addresses. In SMMU pass-through that means physical memory below 4 GiB, outside XNU's window (0x8c0000000 and
up), that nobody else uses. Linux needs no fixed range (its compute-cb sessions translate, and `adsp-rpc-remote-heap` is a movable CMA
pool for the audio PD's static process), so the default comes from the live DT's fixed regions:

| Fact | Source | State |
|---|---|---|
| `cdsp@9d200000`, 25 MiB, `no-map`, in reserved-memory | `docs/hyp-evidence/memory-map-running-dt.txt`, `out/inputs/piano-boot.dtb` | verified (DT) |
| its only user, `remoteproc@32300000` (`qcom,sm8750-cdsp-pas`), is `status = "disabled"` on piano: Ubuntu never ran the CDSP, nothing native maps the range (repository search) | the same DT | verified (DT, source) |
| piano's UEFI keeps it inside `PIL_Reserved` (0x89e00000 + 0x19700000): neither UEFI nor DarwinBoot allocates there | pianoPkg `MemoryMapLib.c` | verified (source) |
| its neighbour `cvp@9cb00000`, the same kind of carveout, is the native console mailbox (written from XNU, read back by UEFI) | `bringup-debugging.md` | verified (hardware) |
| the DSP reaches it through the bypassed stream | stage 2 is the HLOS VM's; no assignment, as the CDSP never authenticates | inferred; the first attach shows it (S2) |

The split has one owner, `package/include/PianoDspMemory.h`; a driver uses its own slice only and changes the split only there:

| Slice | Range | User |
|---|---|---|
| sensors | 0x9d200000 + 8 MiB | PianoSensors' FastRPC arena: the `piano-sensors-rpc-pa` default (2 MiB of calls, up to 6 MiB of `apps_mem` heap pages, PAG's split) |
| audio | 0x9da00000 + 16 MiB | PianoAudio's DMA buffers when it needs them (`audio.md` §10.4), unused until then |
| spare | 0x9ea00000 + 1 MiB | unassigned |

Another address (`piano-sensors-rpc-pa=ADDR`) is taken only when `ps_arena_check` finds it in none of: every reserved-memory region of
the live DT below 4 GiB but the CDSP carveout (among them the console mailbox `cvp@9cb00000` + 7 MiB, the GPU's ZAP `gpu-micro-code@9b09a000`,
PianoVideo's `video@9c300000`, the ADSP's `adspslpi`, `q6-adsp-dtb` and `adsp-mhi`), the firmware's holes, the UEFI regions natively still
in use (the firmware volume with its stacks and heaps, the boot image's "Kernel" area, the DXE heap, `Piano_Log`, `Display_Demura`), the
CDSP carveout's other slices, and XNU's own memory; the host tests check every region of the evidence file is in that table. The
alternative stays a translating context bank with our own page tables (UniXNU's ArmSmmu4K): on piano the HLOS programs the compute-cb
banks (Linux does). The arena is mapped write-combined and published with a barrier before each send.

**Properties** of the `PianoSensors` service (class `PianoSensors` on IOPlatformExpertDevice; match it by class name). They are the
interface for the consumers (the auto-brightness and rotation work) and stay as named here; values are set with `setProperty` (no
notification: poll at most 4 times a second, or the kernel consumer reads them on its own thread).

| Property | Type | Set by | Meaning |
|---|---|---|---|
| `piano-sensors` | number | all levels | the level running (1..4) |
| `piano-sensors-guard` / `-guard-reason` | string | level 1+ | `wait`, `run`, `backoff`, `off` / `none` or why it is off (`sg_reason_name`) |
| `piano-sensors-dsp` | string | level 1+ | PianoPower's health: stage, up, fatal/reports, link and charger generations, quiet, crash reason |
| `piano-sensors-channel` / `-channel-ups` | number | level 1+ | 1 while `fastrpcglink-apps-dsp` is up / how often it came up |
| `piano-sensors-sns` | string | level 1+ | where service 400 is announced (node, port, instance, generation), or not announced |
| `piano-sensors-pack` | string | level 1+ | the pack's verdict: `ok: N entries, B bytes;` and which of sns_reg_config, json.lst, registry, soc0 it holds, or the error |
| `piano-sensors-smmu`, `-arena` | string | level 1+ | the four streams' entries (planned at 1, set at 2); the arena's address and verdict |
| `piano-sensors-rpc` | string | level 1+ | the file service: stage, attaches, calls, opens, missing, reads, refused (writes), declined (guard), bytes, first missing path |
| `piano-sensors-types` | dictionary | level 3 | data type (`registry`, `accel`, `gyro`, `mag`, `ambient_light`, `proximity`) -> `pending`, `none`, `no answer`, or `found N: name (vendor), available A, streaming/on change, rates L-H Hz` |
| `piano-sensors-lux` | number | level 4 | the front light sensor's last reading, integer lux (with each target, else at most every 250 ms) |
| `piano-sensors-light-target` | number | level 4 | the backlight target, permille 0..1000 (`ps_als_step`); absent before the first sample |
| `piano-sensors-edge` | string | level 4 | the panel edge that is up: `top`, `left`, `bottom`, `right`, `none` (`ps_edge_name` of `ps_edge_of`, after SensorsOrient's hold; `none` while the stream is not wanted, failed or off; `none` at start) |
| `piano-sensors-seq` | number | level 4 | moves by one with every change of the three above (0 at start), set after them; an edge change also sends `kIOMessageServicePropertyChange` |
| `piano-sensors-streams` | dictionary | level 4 | `light`, `accel` -> state (`off (display off)`, `enabling`, `on`, `disabling`, `failed`, `no port`, `no service`, `no sensor yet`, `off (the guard)`), the request (`on change` / `N Hz asked`, the hub's rate), sessions, requests, answers, refused, unanswered, rearmed, errors, samples, dropped, and `consumer: ...` (§5c.2) |

The rotation agent applies `ps_rotation_step` itself (SensorsPolicy.c is plain C and links into user space) with PianoKeyboard's
`piano-keyboard-attached` and the user's lock; the backlight takes `piano-sensors-light-target`.

**macOS consumers** (all of it in §5a, code in `package/PianoAuto`).
- Ambient light: the curve (`ps_als_curve`: 0 lux 4 %, 100 lux 24 %, 300 lux 37 %, 1000 lux 57 %, 10000 lux 100 %, battery first),
  smoothing (1 s towards brighter, 4 s towards darker), 3 % hysteresis, at most one target per 250 ms, in the kext; PianoBacklight
  applies it in an automatic mode with the user's offset (the brightness keys shift the curve). The stream is off while the display is
  off or the lid closed and starts afresh after every off/on. macOS's own CoreBrightness needs a built-in panel with Apple's backlight
  service; NativeGopDisplay's panel is external (backlight.md §5).
- Rotation: the kernel publishes the edge that is up (`piano-sensors-edge`, after SensorsOrient's 250 ms hold); the launch agent
  `piano-rotated` settles it (600 ms), keeps rotations 1 s apart, holds 0 deg while the cover is attached (Ubuntu's rule) and sets
  WindowServer's own rotation (verdict R1, §5a.1); PianoTouch follows the rotation read back.
- Proximity, gyroscope, magnetometer, the rear light sensor: unused (§5a.7).

## 5a. The consumers on macOS: rotation, touch, automatic brightness (package/PianoAuto)

Status, 2026-11-11: offline. `package/PianoAuto` is host-tested (`tests/run-host-tests.py`: the rotation policy, the touch transform,
the sensor streams' off/on lifecycle, the light chain, PianoBacklight's automatic mode against its own step ladder; the kernel's
no-FP/no-libc compile) and the agent `piano-rotated` is built (`tooling/k27/build-piano-rotated.sh`); its read-only `--probe` ran on a
Mac with macOS 26.6. Nothing ran on the tablet. The two driver interfaces are built, host-tested and linked, both off by default:
PianoTouch's rotation (`piano-touch-rotate=1`, §5a.3) and PianoBacklight's automatic mode (`piano-backlight-auto=1`, §5a.4); PianoKeyboard
and NativeGopDisplay are unchanged. Independent of the sensor transport: everything but §5a.5 works with a test edge source today.

### 5a.1 Display rotation: the mechanism (verdict)

| Item | Value | Source | State |
|---|---|---|---|
| macOS's sense | `CGDisplayRotation`: degrees clockwise; 90 = the display turned clockwise, its physical bottom now on the left and its top on the right, i.e. the panel's left edge up (SensorsPolicy's default map: TOP 0, LEFT 90, BOTTOM 180, RIGHT 270) | CGDisplayConfiguration.h (public) | verified (header) |
| Public setter | none: CoreGraphics only reads the rotation; display modes are public (`CGDisplaySetDisplayMode`) | SDK headers | verified |
| IOFramebuffer era | a framebuffer with `kIOScaleCanRotate` in its scaler features rotated scaled modes (`kIOScaleRotateFlags`, set by `IOServiceRequestProbe(kIOFBSetTransform)`): Intel GPUs' family, not IOMobileFramebuffer's, so not this display | IOGraphicsTypes.h | verified (header) |
| System Settings' setter | MonitorPanel.framework: `+[MPDisplayMgr sharedMgr]`, `-displayWithID:` (int), `-[MPDisplay canChangeOrientation]` (BOOL), `-orientation` (int), `-setOrientation:` (int), `-hasRotationSensor` | Objective-C runtime names and type encodings, macOS 26.6 | verified (names, resolve); macOS 27: needs-boot |
| Below it (names only, not called) | SkyLight `SLSSetDisplayRotation`; CoreDisplay `CGXSetDisplayRotation` (WindowServer's side); IOMobileFramebuffer.framework `IOMobileFramebufferSetRenderingAngle`, whose kernel side is `IOMobileFramebufferLegacy::setRenderingAngle(float*)` (slot 353, the family's own; NativeGopDisplay does not override it); `rotate_surface` (slot 279) | exported symbols; the KC's IOMFB names | verified (names); behaviour not read |
| Offered for this display? | `canChangeOrientation` is WindowServer's answer; on Apple silicon external displays get Rotation, and NativeGopDisplay's display is external | macOS behaviour | inferred; R0 |
| Rotated frames | expected: WindowServer composites rotated into the display's own 3200x2136 surfaces and tells the framebuffer the angle (the name `setRenderingAngle`): the copy and later flips unchanged. A transposed 2136x3200 surface (the display to rotate) would show in NativeGopDisplay's existing `swap layers 0x1: 0: 2136x3200` line | names above | inferred; R0 |
| DPU rotation | SM8750's pipes (VIG0-3 `VIG_SDM845_MASK_SDMA`, DMA0-5) have no inline rotation (only SC7280's VIGs, NV12 UBWC, up to 1088 lines); every pipe flips: SRC_OP_MODE +0x38 (rect 1 +0x17c) bit 13 FLIP_LR, bit 14 FLIP_UD, so 0 and 180 degrees only | msm `dpu_hw_catalog.c`, `catalog/dpu_12_0_sm8750.h`, `dpu_hw_sspp.c` | verified (source) |

**Verdict: R1, WindowServer's own rotation, set by a launch agent through System Settings' setter; no kernel change.** It needs no
display-driver change, keeps PianoDpu's zero-copy plan intact (no 90-degree pipe is needed when WindowServer composites rotated), and is
read back with a public call. **R2** (NativeGopDisplay rotates its copy, portrait timings chosen with the public `CGDisplaySetDisplayMode`)
only if R0 shows `canChangeOrientation NO` or transposed swaps: it changes the timing list WindowServer keeps per display, makes every copy
a transpose (slower than today's 39 ms row copy) and leaves PianoDpu without zero-copy for 90/270. Its spec: §5a.6. **R3**: none.

### 5a.2 piano-rotated (the agent)

```
PianoSensors  piano-sensors-edge (top|left|bottom|right|none) when piano-sensors-seq moves --+
PianoKeyboard piano-keyboard-attached (0/1)        + kIOMessageServicePropertyChange --+--> AutoRotate (ps_rotation_step inside)
the user      defaults com.unixnu.piano-rotated Locked; notifyutil -p ...prefs     --+        |
WindowServer  CGDisplayRotation (read back) + the display reconfiguration callback   --+        v
              <-- -[MPDisplay setOrientation:] (by name, encodings checked first)    <-- rotate (level 2)
PianoTouch    <-- piano-touch-rotation (setProperties): the rotation shown            <-- touch (level 3)
```

A global LaunchAgent (`daemon/com.unixnu.piano-rotated.plist`: LoginWindow and Aqua sessions, since WindowServer's display services are
per session; a system daemon would not reach them), event driven for the cover and WindowServer; PianoSensors sets its properties
without a notification (§5), so its `piano-sensors-seq` is read every 250 ms only while it can matter (PianoSensors at level 4, the cover
not attached; every 5 s while the display sleeps) and PianoSensors is found by class even unregistered; other timers only while a
settle, hold or read-back is due;
arm64, ad-hoc signed, `/usr/local/libexec/piano-rotated`. Policy (`AutoRotate.h`): an edge settles 600 ms; rotations at least 1 s
apart; cover attached: 0 at once and held (a rotation chosen while attached is turned back); detached: a 1.5 s hold, then an edge that
settles afresh (contacts bounce while the cover goes on or off); every rotation read back within 2 s, else backoff 2 s doubling to
60 s and, after 5 failures in a row, off for the session; a rotation made in System Settings is adopted and holds until the tablet is
turned to another edge; the user's lock; only on the console's session with the display awake.

| Boot argument (read by the agent) | Default | Meaning |
|---|---|---|
| `piano-rotate=N` | 0 | 0 the agent exits (launchd does not restart it); 1 observe (inputs and decisions logged); 2 + WindowServer's rotation; 3 + PianoTouch told |
| `piano-rotate-map=T,L,B,R` | 0,90,180,270 | degrees per edge up (fix after the four-edge test) |
| `piano-rotate-settle=MS`, `-interval=MS`, `-detach=MS` | 600, 1000, 1500 | the policy's times |
| `piano-rotate-cover=N` | 2 | 2 held at 0 while attached; 1 turned to 0 at attach only; 0 the cover ignored |
| `piano-rotate-test=1` | 0 | the edge from the preference `TestEdge` (`defaults write com.unixnu.piano-rotated TestEdge -int 1`, then `notifyutil -p com.unixnu.piano-rotated.prefs`) instead of PianoSensors |

Command line (USB root shell): `--probe` (displays, the setter, `canChangeOrientation`, the three drivers' properties; changes nothing),
`--set DEG [--touch]` (one rotation, read back), `--level N`, `--stderr`. Log: `log show --last 5m --predicate 'subsystem ==
"com.unixnu.piano-rotated"'` (`start: level ...`, `edge up: left`, `keyboard cover attached`, `wants 90 degrees: the tablet turned`,
`rotate 0 -> 90 degrees: asked`, `display 0x.. shows 90 degrees`, `PianoTouch told 90 degrees: taken`; failures: `MonitorPanel has no
... (another macOS build?)`, `WindowServer offers no rotation ... (canChangeOrientation NO)`, `the rotation asked for did not show`).

### 5a.3 PianoTouch: the rotation input (built: `piano-touch-rotate=1`, touch.md §5)

- Off by default: without `piano-touch-rotate=1` both inputs are refused as before and every report is unchanged.
- State `PatRotation fRotation`. Inputs, both only recording (`pat_request`), never waiting:
  `callPlatformFunction("PianoTouchRotation", false, (void *)(uintptr_t)degrees, (void *)source, 0, 0)` for kernel callers, and
  `setProperties({"piano-touch-rotation": 0|90|180|270})` for the agent, allowed for root (`kIOClientPrivilegeAdministrator`) or the
  console user (`kIOClientPrivilegeConsoleUser`: the agent runs as the logged-in user); other values `kIOReturnBadArgument`, other
  callers `kIOReturnNotPrivileged`. Published: `piano-touch-rotation` (the rotation in use).
- In `contact()`: `pat_latch(&fRotation, 1)` at each touch-down; `send()` builds `rotatedTabletReport(tip, x, y, kPanelW, kPanelH,
  fRotation.shown, report)` (`TouchHid.h`: `pat_rotate`, then `tabletReport` over the rotated size). The degrees are CGDisplayRotation's
  (the agent sends what it read back); a change takes effect at the next touch-down, never mid-drag. At 0 the bytes are the same as
  before (host test). One log line per change. The agent needs `piano-touch-rotate=1` with `piano-rotate=3`.

### 5a.4 Automatic brightness: PianoBacklight's automatic mode (built: `piano-backlight-auto=1`, backlight.md §5)

- Boot argument `piano-backlight-auto=1` (default 0: targets refused with `kIOReturnUnsupported`, nothing changes).
- Two inputs, the same effect: PianoSensors' `piano-sensors-light-target` (with `piano-sensors-seq`, `piano-sensors-lux`; §5's table),
  read on PianoBacklight's 250 ms poll and inert until PianoSensors publishes it; and the platform function below.
- `callPlatformFunction("PianoBacklightAmbient", false, (void *)(intptr_t)permille, (void *)source, (void *)(uintptr_t)lux, 0)`:
  permille of the code range (1000 = 2047), the light policy's output; -1 leaves automatic mode with the code as it is. Records the
  newest and wakes the thread like `PianoBacklightStep`; `kIOReturnNotReady` before its lock exists. `PianoBacklightClient.h` has the
  name and an `ambient()` helper.
- `Controller` takes `pianoauto::Ambient` (`ambient()`, `automatic()`, and in `step()`/`percent()`): a target applies `code()`; a step in automatic mode
  shifts the curve by one of its own steps (x 2^(1/3) linear, 1/16 exponential) and the offset stays for the boot, a press always a
  visible step unless at an end; `piano-backlight-percent` (administrator) switches to manual; `piano-backlight-auto` 0/1 as a
  property leaves or re-enters (the last target at once, the offset kept). Written only on a change, nothing while dark (the newest
  code lights the wake).
- Published: `piano-backlight-auto` (automatic now), `piano-backlight-offset`, `piano-backlight-ambient-permille`, `-ambient-lux`.
- Log: ambient writes folded into one line per 10 s (`ambient: N targets in S s, lux A..B, target P..Q permille, offset K, code X -> Y`;
  failed accesses still logged at once); key steps in automatic mode `step +1 from ... in automatic mode: offset 0 -> 1, code 757 -> 954`.

### 5a.5 PianoSensors' side (its kext at S4: `AutoStream.h`, `AutoLight.h`; built, §5c)

The outputs are §5's level-4 properties as landed: `piano-sensors-light-target` (permille), `piano-sensors-lux`, `piano-sensors-edge`
(`ps_edge_name`), `piano-sensors-seq`, set without a notification but for the edge (a `kIOMessageServicePropertyChange` with each
change); both consumers poll them at most 4 times a second. The kext calls `registerService()` in `start()` at every level, so
matching (PianoBacklight's `copyMatchingService`, the agent's notifications) finds it; both still find it by class too. As built
(§5c), these notes hold with three precisions: the light's consumer is PianoBacklight's automatic mode read from its boot arguments
and properties (the kext sends no target through the platform function, only the -1 below); each stream has its own QMI client; the
accelerometer also waits for its consumer (piano-rotated: `piano-rotate>=1`).

- Ambient light: one `PalLight`; conditions: `piano-sensors-als` and PianoBacklight taking targets, NativeGopDisplay's
  `piano-display-power`, PianoKeyboard's `piano-keyboard-hall` (unknown counts as open). Each `pas_action` goes out only with
  `sg_may_request`'s token (enable = 514 on change, disable = 10, on the light's own QMI client), answers matched by QMI transaction id
  to `pas_response`, indications to `pal_event` (data[0]), a timer at `pal_deadline` to `pal_tick`; each target into
  `piano-sensors-light-target` (or to `PianoBacklightAmbient`); `PAS_FAILED` sends -1 once (the platform function). The rules make readings resume after every off/on (Ubuntu v1.1.1): one request
  outstanding, no nested close, the first event kept even before the enable's answer, a re-arm when it never comes, a fresh start
  from it (no fade from the level before the display slept).
- Accelerometer: one `PasStream` (513 at 10 Hz), wanted with the display on, the cover detached (`piano-keyboard-attached` 0 or
  absent) and `piano-sensors-rotate`; samples through `ps_accel_sample` (a `PAS_EV_FIRST` restarts `sno_init`); on each new state
  `piano-sensors-edge` = `ps_edge_name(ps_edge_of(state))` (`none` for flat, face up/down, unknown, the stream off) and
  `piano-sensors-seq` moved. A `messageClients(kIOMessageServicePropertyChange)` with it would let the agent stop polling.
- PianoKeyboard: nothing new (`piano-keyboard-attached` and its messages exist, keyboard.md §6c).

### 5a.6 NativeGopDisplay and PianoDpu

R1 asks nothing of either. R2's spec, only if R0 calls for it, behind `native-gop-display-rotate=1` (byte-identical without): override
`setRenderingAngle(float*)` (decode its bits without FP, log the first 8 calls and each change, publish
`native-gop-display-rendering-angle`, then call the family's); copy a main surface of the transposed size rotated by that angle (or by
`native-gop-display-rotation` set by the agent, root or console user) through a 64-row tile (256-byte runs read into a cached buffer,
whole framebuffer rows written); only with `canChangeOrientation NO`, a second timing 2136x3200 that the agent chooses with
`CGDisplaySetDisplayMode`. PianoDpu (B1b): R1's surfaces flip as today; a 180-degree flip is FLIP_LR|FLIP_UD with the two halves'
sources swapped (the left mixer fetches the surface's right half); 90/270 always go to the copy.

### 5a.7 Unused: proximity, gyroscope, magnetometer, the rear light sensor

Proximity has no consumer on a Mac: no call or ear detection, the screen goes off by display sleep and the folio's hall (PianoBacklight's
`piano-backlight-lid`); waking on approach would keep a stream on while the display sleeps (DSP traffic, sensor power) for little gain.
It is discovered and listed at S2, never streamed. The gyroscope could gate rotation while the tablet turns (the settle time does that),
the magnetometer has no macOS consumer, the rear light/flicker sensor (`ambient_light_back`) is not the one facing the user.

### 5a.8 Test plan (attended RAM boots, the GPU profile so WindowServer runs; nothing needs the sensors until R3)

Staging (the A830 path's seed plus the agent; `PIANO_SEED`/`PIANO_TRUST` make build-native-image.sh take it). The userland
pipeline (`configs/userland.toml`, `tooling/userland/stage.py`, `docs/userland.md`) installs the same agent with the rest; the
recipe below stages it alone, with the log loop:
```sh
tooling/metal/build-turnip-a830.sh && printf 'sysmem\n' > out/turnip-a830/tu-debug
R=$(tooling/k27/build-piano-rotated.sh); A=private/var/piano/rotated
python3 tooling/k27/stage-soft-metal.py --out out/rotated/stage --file out/turnip-a830/tu-debug:private/var/ngpu/debug/tu-debug \
  --file $R/piano-rotated:$A/piano-rotated --file $R/com.unixnu.piano-rotated.plist:$A/com.unixnu.piano-rotated.plist \
  --file $R/install-piano-rotated.sh:$A/install-piano-rotated.sh --boot-run "sh /$A/install-piano-rotated.sh" \
  --log-stream 'subsystem == "com.unixnu.piano-rotated"' out/turnip-a830/NativeAdrenoMetal.bundle
PIANO_SEED=out/rotated/stage/scratch-data.img PIANO_TRUST=out/rotated/stage/kernel.macho.trustcache PIANO_GPU=2 PIANO_VMM_PRESENT=0 \
  PIANO_RESET_AFTER=900 PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-usb-hid=1 piano-usb-shell=1 piano-rotate=2 \
  piano-rotate-test=1" tooling/k27/build-native-image.sh
```

| Step | What | Pass |
|---|---|---|
| R0 | root shell: `piano-rotated --probe`; `--set 90`, `--set 180`, `--set 0` | MPDisplay found, `canChangeOrientation 1`; each `... -> 90 degrees: shown`; the thumbnail rotated; `swap layers` stays `3200x2136` (R1 holds; `2136x3200`: R2); no WindowServer exit |
| R1 | `piano-rotate=2 piano-rotate-test=1` (+ `piano-keyboard=2`): TestEdge 1, 3, 2, 0; attach and detach the cover; Locked true/false | each edge's rotation 0.6 s after its notification; attach: 0 at once, held; detach: none for 1.5 s; locked: none; a System Settings rotation adopted |
| R2 | `piano-rotate=3 piano-touch=2 piano-touch-rotate=1`: at each rotation the four corners and the centre | the pointer under the finger (about 10 px); a drag never jumps at a rotation |
| R3 | with PianoSensors S4 (§8 S4/S5): `piano-rotate=3` without `-test`; PianoBacklight with `piano-backlight-auto=1` | the four edges upright with touch; attached: normal; the backlight follows a lamp within 2 s; 20 display sleep/wake cycles each give readings and the room's level at once |

### 5a.9 Risks

1. The setter is private: pinned by name and encoding, checked before each run; a changed macOS refuses cleanly (R2 then).
2. `canChangeOrientation` may be NO for this IOMFB display, or WindowServer may hand it transposed surfaces: R0 says which; R2.
3. Rotated composition cost: with NativeSoftMetal (software compositing) a rotated desktop costs CPU per frame; on the A830 path it
   is a GPU pass. Measured in R0 (the copy line's frame rate and CPU).
4. Absolute pointers: the transform assumes macOS stretches the HID tablet's range over the rotated display; if macOS rotates it
   itself, R2's corners show it: then `piano-rotate=2` (touch not told).
5. PianoTouch's property is writable by the console user (the agent is not root after login): a user process could remap touch to one
   of four rotations; bounded, documented. A root daemon cannot reach WindowServer's per-session display services.
6. AMFI: the agent is ad-hoc signed like `pianousb-shell`; if it is refused, its CDHash (printed by the build) goes into the trust
   cache as the plugin's do. The login window's agent loads only if the boot job installs it before loginwindow starts; otherwise at
   the next login.
7. Sensors (S4): the hub's first event on enable is inferred (the re-arm covers it); an under-display light sensor sees the panel
   (question 9); the edge's axes are inferred (`piano-rotate-map`, `piano-sensors-axes`).

### 5b. The pack, step by step (the owner's own tablet; nothing private is committed)

Ubuntu is no longer on the tablet (its root became macOS's), so the pack is made on the Mac from two local sources:
- **this unit's registry**: the backup made before the switch, `CRITICAL-BACKUP/sensor-registry.tar` (Ubuntu's private seed: the
  outer registry directory with `registry/`, `sns_reg_version` and `parsed_file_list.csv` under `usr/share/piano/sensors/registry-private/`).
  Private: never committed, never printed (the tool reports only a count), never another unit's;
- **the stock ROM's sensor configuration**: OS3.0.303.0.WPYCNXM's 66 ODM/vendor files as the Ubuntu project keeps them,
  `xiaomipad-8pro-mainline/firmware/sensors` with `source.json` (each file's device path and sha256; the tool checks every hash). The
  owner's own ROM image on this Mac is incomplete (`rollback-os3.0.303/`, a sparse download marked do-not-use), so it is not used; the
  manifest-checked copy is the same build's files. Reading `vendor_a`/`odm_a` from `super` at run time through NativeUfs (LP metadata,
  EROFS) is not needed while this copy matches.

```sh
python3 package/PianoSensors/tools/make-sensor-pack.py \
  --registry-tar "$HOME/Downloads/XiaomiPad8Pro-Ubuntu/CRITICAL-BACKUP/sensor-registry.tar" \
  --rom-config "$HOME/Downloads/XiaomiPad8Pro-Ubuntu/xiaomipad-8pro-mainline/firmware/sensors" \
  --out out/sensors/pack.bin
python3 tooling/persist/build-esp.py --profile base --sensor-pack out/sensors/pack.bin    # plus the usual --arg/--default options
```

Run 2026-11-11 (on the owner's sources, offline): `ROM configuration: 66 files of OS3.0.303.0.WPYCNXM, each matching its sha256`,
`persist registry: 228 files, 99033 bytes (private: not listed)`, `out/sensors/pack.bin: 294 files, 338020 bytes`; the kernel's checks
(header, table, names) accept it and find sns_reg_config, json.lst and the registry. No soc0 values: no source holds them, none is
invented; level 2's log shows whether the PD asks for `/sys/devices/soc0` (then a `--soc0` file from the SoC's identity).

**A RAM test image that carries the pack** (built 2026-11-11, host-tested; no ESP write, nothing committed): `PIANO_SENSOR_PACK=PACK`
for `tooling/bootimg/build-ram-images.py` (also through `build-native-image.sh`, which then makes the images' ADT with the GpuAddon slot
as `out/adt/piano-k27-addon.adt`, and `tooling/k27/build-sensor-test-images.sh`, §8.1) puts the pack, zlib-compressed, as the one entry
`sensors/piano-sensors.pack` of UniXNU's boot addon container (`package/PianoSensors/tools/sensor_addon.py`; the layout of UniXNU's
`scripts/make-gpu-addon.py`) into the payload as `Payloads/kernel.macho.gpuaddon`. DarwinBoot copies that file into the kernel's memory
and fills the ADT's `/chosen/memory-map` `GpuAddon` {address, size} (the k27 kernel has no GPU addon of its own: the slot is free
natively). PianoSensors (`SensorsAddon.c`) checks the container whole, finds the entry, inflates it with libkern's zlib, checks the pack
as it checks the ESP's and takes it before the ESP: `piano-sensors-pack` then ends `from the RAM image` (else `from the ESP`); a carried
pack that does not check is not replaced by the ESP's (the image is wrong, and says so). DarwinBoot refuses a slot without the file and
a file without the slot, so `build-ram-images.py` refuses either mismatch and `build-esp.py` refuses an ADT with the slot: the installed
system's ADT never has it. Only a pack under `out/` or `private/` is taken, the record keeps only its sha256, and the images then hold
this unit's calibration: they stay in the ignored `out/` and are never shared.

The tool writes only under this repository's `out/` or `private/` (anything else is refused), with mode 0600; it refuses a link or a
special file in the registry, a second registry directory, a ROM file whose sha256 differs, a ROM path outside the sensor trees, a serial
number in soc0. `build-esp.py --sensor-pack` checks the pack (magic, size, at most 4 MiB), puts it at
`EFI/UniXNU/piano-sensors.pack` and records only its sha256 in `out/persistent/<name>.json`; the ESP image then holds this unit's
calibration (keep it local, never share it). Writing the ESP to the tablet is the maintenance session's step (`maint-session.py`,
`persistent-boot.md`), done by the owner. A RAM-image boot reads the pack from the installed ESP as well.

For a tablet that still runs Linux, `--dump DIR` takes archives made there (read only; `persist` is never mounted writable; the
archives' member names are the DSP's paths):

```sh
mkdir -p ~/piano-sensor-dump && cd ~/piano-sensor-dump
sudo mkdir -p /mnt/persist-ro && sudo mount -o ro,noload /dev/disk/by-partlabel/persist /mnt/persist-ro
sudo tar -C /mnt/persist-ro --transform 's,^,mnt/vendor/persist/,' -cf persist-sensors.tar sensors
sudo umount /mnt/persist-ro
# the ROM's sensor configuration as Ubuntu's piano-sensors package installed it (the same for every unit of that ROM build)
tar -C /usr/share/piano/sensors --transform 's,^vendor/,vendor/etc/sensors/,' -cf vendor-etc-sensors.tar vendor
tar -C /usr/share/piano/sensors --transform 's,^odm/,odm/etc/sensors/,' -cf odm-etc-sensors.tar odm
for n in soc_id revision machine family platform_subtype platform_subtype_id platform_version hw_platform; do
  [ -r /sys/devices/soc0/$n ] && echo "$n=$(cat /sys/devices/soc0/$n)"; done > soc0.txt
sudo chown "$USER" *.tar && chmod 600 *
```

then `make-sensor-pack.py --dump out/sensors/dump --out out/sensors/pack.bin --list` (`--list` prints sizes and paths only, the
registry as a count). The tool warns when sns_reg_config, json.lst or the persist registry is missing (without the registry the DSP falls
back to the ROM's generic calibration).

## 5c. Level 4: the streams (`piano-sensors=4`, S4), as built

Status, 2026-11-11: built, host-tested (`tests/streams.c`, a simulated hub sending the service's message shapes; PianoAuto's
`stream_tests.h`) and linked; not run. `SensorsStream.c` (portable C on the main thread) drives PianoAuto's `AutoStream.c` and
`AutoLight.c`, compiled into the kext from their package: the off/on rules the consumers' side was written for are now the producer's
own. Inert below `piano-sensors=4`: no stream port is opened, the kext does not join the power tree, no level-4 property is set.

### 5c.1 The protocol (facts; libssc and Android as the specification, no code)

| Item | Value | Source | State |
|---|---|---|---|
| A client per stream | each stream on a QRTR port of its own on PianoPower's router (`piano_qrtr_open`), so a QMI client of its own at the hub (its id in TLV 0x10 of every response); the level-3 list keeps its port: 3 of the 8 ports | libssc (one QMI client per sensor), Android's sensors HAL (one connection per sensor), `PianoAdspContract.h` | verified (source); answers question 6 |
| Which sensor | the default SUID of `ambient_light` / `accel` from level 3's lookup (default only, with updates), taken once its attributes are read or given up, not when `available` is false; a new default sensor while its stream runs: the stream's port is closed and a new one starts clean | SSC, PAG | verified (source) |
| Enable, light | message 514 (`sns_std_sensor`'s on-change config), request `{}` (no payload): the sensor reports when its value changes; if its attributes say `stream_type` 0 (streaming), 513 at its lowest advertised rate instead | SSC protos, libssc; Android (the light is an on-change sensor) | verified (source) |
| Enable, accel | message 513, request `{payload {sample_rate 1: 10.0f}}` (float bits 0x41200000); the driver runs its nearest rate at or above and says which in the physical configuration event (768, field 1), logged; `batching {batch_period µs}` only with `piano-sensors-accel-batch` (off: as libssc) | SSC protos, libssc | verified (source); the rate chosen: needs-boot |
| Disable | message 10, request `{}`, to the same SUID on the same client | SSC protos, libssc | verified (source) |
| Delivery | `susp_config {processor 1 (APSS), delivery 0 (wakeup)}` as level 3 and libssc; both streams are off before every system sleep anyway (§5c.4) | SSC, libssc | verified (source) |
| Samples | report 0x21 `{TLV 0x01 client id, TLV 0x02 sns_client_event_msg {suid, events {msg_id 1025, timestamp (19.2 MHz), payload sns_std_sensor_event {data (repeated float, packed or not), status 0..3}}}}`; light: `data[0]` lux (the light drivers add a raw count, unused); accel: `data[0..2]` m/s² in the SSC frame (+9.81 on the axis that points up); a report may carry several events (batching) | SSC protos, libssc, SUN's readings | verified (source) |
| Other events | 768 (the rate: logged), 130 error (a failure, §5c.2), 129 flush, 133 resampler, 1022 calibration: counted, unused | SSC protos | verified (source) |
| First event | an on-change sensor reports its current value when enabled | Android's on-change semantics, Ubuntu's readings after each enable | inferred; the re-arm covers it (5a.9 risk 7) |
| A client gone | `piano_qrtr_close` sends DEL_CLIENT for the port; a QMI service drops a gone client's state, as when a Linux client's socket closes | Linux `qrtr` and `qmi_interface.c` (read for behaviour), `QcomQrtr.h` | verified (source); the hub's side: needs-boot |

### 5c.2 When a stream runs, and the off/on rules

| Stream | Wanted while | Read from |
|---|---|---|
| light | a consumer (`piano-sensors-als`: 1, the default, PianoBacklight's automatic mode: `piano-backlight=2 piano-backlight-auto=1` and not manual by the user's choice, i.e. not its published `piano-backlight-auto` false once it has taken a target; 2 always; 0 never), the display on, the lid open, no system sleep | the boot arguments, PianoBacklight's properties, NativeGopDisplay's `piano-display-power` (always published; unknown before the first counts as on), PianoKeyboard's `piano-keyboard-hall` (unknown counts as open) |
| accel | a consumer (`piano-sensors-rotate`: 1, the default, piano-rotated reads the edge: `piano-rotate>=1` and not `piano-rotate-test=1`; 2 always; 0 never), the display on, the cover detached (`piano-keyboard-attached` not 1), no system sleep | the boot arguments, NativeGopDisplay, PianoKeyboard |

The services are found by class (registered or not) every 2 s for two minutes, then every 30 s; their properties are read on each pass
of the main thread, and their property-change messages (NativeGopDisplay's display power, PianoKeyboard's cover and hall) wake it at
once, so a wake's first light sample does not wait for the next poll. The rules are AutoStream's (Ubuntu's v1.1.1 fix as a rule, §3):
one request outstanding per client; an enable never followed by a disable before its answer, nor a disable by an enable; the first event
kept also when it overtakes the enable's answer, and marked so that the light starts afresh from it; events while off, while a disable
is out or while the hub's state is unknown are dropped; no first event within 3 s: re-armed (disable, enable), at most twice per period
of wanting; no answer within 2 s, or an error event (130): a failure and the hub's state unknown, so a disable goes first, then a fresh
enable; failures back off 1 s doubling to 32 s, six in a row end the stream for the boot. Every request takes a token of the guard (§7
rule 4) and every outcome is told to it (a refusal at the port or by the hub, a missing answer and an error event are failures), so the
guard's backoff paces the retries and its strikes end them; the guard itself is unchanged.

Outputs (§5's table): each light sample through `ps_als_step` (AutoLight: the first sample of every session at once, then the
smoothing at most every 250 ms and only while it moves) into `piano-sensors-light-target`, the lux into `piano-sensors-lux` (with each
target, else at most every 250 ms); each accelerometer sample through `ps_accel_sample` (the axis map `piano-sensors-axes`, SensorsOrient's
250 ms hold) into `piano-sensors-edge`; `piano-sensors-seq` moves by one with every change and is set after the values, an edge change
also sends `kIOMessageServicePropertyChange`. The consumers are unchanged: PianoBacklight's 250 ms poll takes the target (§5a.4),
piano-rotated reads the edge on each sequence move or notification (§5a.2).

### 5c.3 Battery

- The lowest report rates: the light on change (nothing while the room stays the same); the accelerometer at 10 Hz, the rotation's need
  (the driver's nearest rate at or above, logged); nothing else streams (proximity, gyroscope, magnetometer, the rear light: §5a.7).
- A stream runs only while its consumer needs it: the light with the automatic brightness on, the display on and the lid open; the
  accelerometer only with the rotation agent, the display on and the cover detached (the docked landscape needs no sensor). Each is
  disabled at the main thread's next pass after its condition goes.
- Before every system sleep both are off (§5c.4); after the wake they start afresh only if wanted.
- The main thread polls the conditions every 250 ms while the display is on or a stream is busy, every second at rest with the display
  off (the display's and the cover's change messages wake it at once); the engine itself keeps no timer at rest (a deadline only for an
  answer due, a first event due, a smoothing step).
- Stream on/off and failure lines go through a limiter per stream (8 lines, then one per 30 s, the lines left out counted in the next
  one); `piano-sensors-streams` keeps the complete counters.

### 5c.4 Teardown: display sleep, system sleep, the guard's strike

- The display sleeps, the lid closes, the cover attaches, the consumer goes: the stream's disable goes out (with the guard's token) and
  its late samples are dropped; the light keeps its last target (PianoBacklight lights the wake with it, then the new session's first
  sample gives the room's level at once), the edge goes to `none`.
- System sleep (`package/include/PianoPowerContract.h`, docs/power.md §4): at level 4 the kext joins the power tree once its main
  thread runs. `setPowerState(kOff)` asks the thread to disable both and is acknowledged once they are off, within 800 ms (it returns
  2 s to IOPMrootDomain); a stream not off by then (no answer, or the guard held the request) has its port closed (DEL_CLIENT: the hub
  drops a gone client's requests), unless PianoPower asks for quiet (then nothing is sent and the stream stays on through the sleep,
  logged). `kOn` lets them start afresh (new sessions; a closed port is opened again). mach_absolute_time stands still while suspended,
  so no timeout runs out across it.
- The guard's strike (§7 rule 3): nothing more is sent; the edge goes to `none`; PianoBacklight is told to leave automatic mode once
  (`PianoBacklightAmbient` -1, the code as it is: the keys work as without automatic mode), if a target was ever given; every later
  packet is ignored; the ports that carried a request are closed only while the DSP may still be told (the strike was ours or the
  charger domain's restart, the DSP up without a fatal sign or crash reason, the charger domain up, no quiet); a dead or dying DSP is
  never touched. A sleep under way is acknowledged at once.

### 5c.5 Lines

`PianoSensors: ` then: `light: QRTR port 24 for service 0x190 (its own QMI client)`, `light: the service is there generation=1`,
`light: sensor 0x..., on change`, `light: on (on change) session=1`, `light: first sample: lux=153 session=1`, `light: off (display
off) session=1 samples_total=12`, `accel: on (10.000 Hz asked) session=1`, `accel: the hub runs it at 12.500 Hz`, `accel: first sample:
x=8.490 y=-0.080 z=4.750 m/s^2 session=1`, `accel: edge top`, `accel: off (cover attached) ...`. Failures: `... the hub refused the
enable error=N`, `... no answer within 2000 ms: the hub's state is unknown (a disable goes first)`, `... error event error=N: a failure;
...`, `... no first event within 3000 ms: re-armed (disable, then enable)`, `... failed for the boot (6 failures in a row); its outputs
at rest`. The sleep: `streams: system sleep: disabling them`, `streams: ready for the sleep after 40 ms` (or `(ports closed)`),
`streams: awake`. The strike: `... the guard is off: nothing more is sent, every packet is ignored, the outputs at rest`, `light:
PianoBacklight told to leave automatic mode: 0x0`, `light: QRTR port 24 closed`.

## 6. Boot args

| Argument | Default | Values |
|---|---|---|
| `piano-sensors=N` | 0 | 0 nothing (probe declines); 1 read-only survey; 2 + the FastRPC file service; 3 + the SNS sensor list; 4 + the streams (§5c); all built |
| `piano-sensors-rpc-pa=ADDR` | 0x9d200000 | the arena's physical address (8 MiB; default the CDSP carveout's sensors slice, `PianoDspMemory.h`; another address only outside every known user, `ps_arena_check`; 0: no file service); level 2 runs no file service without one |
| `piano-sensors-bank=N` | 3 | the context bank (3..8); its streams 0x1000+N, +0x80, +0x40, +0x60 |
| `piano-sensors-smmu=0` | 1 | leave the SMMU alone at level 2 (the streams must bypass already) |
| `piano-sensors-settle=S` | 20 | seconds of the charger's newest generation before the first request |
| `piano-sensors-nopack=1` | 0 | level 2 with an empty pack (every open fails and is logged) |
| `piano-sensors-rpc-frame=N` | 1 | the file service's first framing (§4): bits 1 Linux's metadata page with every invoke, 2 Linux's ctx numbering, 4 a thread id of its own; 0 PaganiSensors' framing (the 2026-11-14 control: with the ladder, the same session then goes on with 1) |
| `piano-sensors-rpc-ladder=0` | 1 | an init2 answered with an error is not tried again with the next framing |
| `piano-sensors-rpc-deadline=S` | 15 | the attach answered -> listening at most (5..45 s); then the session is released (§7 rule 7) |
| `piano-sensors-channel=0` | 1 | level 1 does not register the FastRPC channel |
| `piano-sensors-delay=S`, `-log=0..2` | 0, 1 | wait first; log volume (2: every SNS line) |
| `piano-sensors-als=0\|1\|2` (level 4) | 1 | the light stream: 1 while PianoBacklight's automatic mode takes targets (`piano-backlight=2 piano-backlight-auto=1`, not manual by the user's choice); 2 always (lux without a consumer: S4); 0 never |
| `piano-sensors-rotate=0\|1\|2` (level 4) | 1 | the accelerometer stream: 1 while piano-rotated reads the edge (`piano-rotate>=1`, not `piano-rotate-test=1`); 2 always (S4); 0 never |
| `piano-sensors-accel-hz=N` (level 4) | 10 | the rate asked (1..100 Hz); the driver runs its nearest at or above (logged) |
| `piano-sensors-accel-batch=MS` (level 4) | 0 | the hub may hold accelerometer samples this long (`batch_period`; 0: none, as libssc; at most 1000) |
| `piano-sensors-axes=a..` (level 4) | `a+y+x+z` | the accelerometer map (SensorsFixed text), for the four-edge test; a text that is not a map: the default, logged |

The consumers' own arguments: `piano-rotate*` (the agent, §5a.2) and `piano-backlight-auto=1` (PianoBacklight, §5a.4).

## 7. Kernel component, and the safety story

Component `PianoSensors` (`com.unixnu.driver.PianoSensors`, `tooling/k27/kext/PianoSensors.json` naming `com.unixnu.driver.PianoPower`
and `com.unixnu.driver.NativeUfs` as libraries), listed after both in `configs/k27-native-kexts.txt`; built by
`tooling/k27/build-native-piano-sensors.sh` (host tests, the copies of FatVolume and SmmuStream checked against their originals, the
objects, the import check: 354 kernel imports, 9 from the libraries). Two threads (`piano-sensors`: health, guard, SNS client, the
streams, properties; `piano-sensors-rpc`: the file service); PianoPower's callbacks only record and wake them. At level 4 the kext
joins the power tree once its main thread runs (`setPowerState`, §5c.4). Rules (`SensorsGuard.c` and `SensorsHealth.h`, host-tested;
level 4 adds none: its requests take the same tokens and count as the same failures, and after a strike it sends nothing more):

1. A client only: never start, stop or restart the DSP, never vote its rails or touch RPMh/AOSS; no re-attach after a failed attach in
   the same boot (Ubuntu's `Restart=no`).
2. Charger first: nothing is sent until the DSP is up, `charger_pd` is up and its newest generation has been up for 20 s, and while
   PianoPower asks for quiet.
3. One strike: the SMP2P fatal bit now or any fatal report since the start (`fatal_reports`), any SMEM crash reason, a new charger
   generation or the charger domain not up, a new link generation or the DSP not up once we started, or a health report of another
   contract version turns the sensors off for the rest of the boot. Before the first request a charger restart only starts the
   settling over.
4. Rate: 8 requests at once, 4 per second (each bring-up call of the file service and each SNS request takes one); a failure (a
   refused or unanswered attach, a refused, unanswered or erroring SNS request) backs off 1, 2, 4 ... 64 s; 6 failures in 10 minutes: off.
5. The DSP's file calls are answered at once from memory; when off, each is still answered, with an error (a silent listener blocks a
   PD thread, which `sns_watchdog` kills the DSP for); more than 3000 file calls in a minute (a PD in a loop): off.
6. Read only: persist and the pack are never written; no per-unit value is logged at default levels; only the allowlisted soc0 names
   (no serial number) are served.
7. The sensor PD is never left waiting (2026-11-14, §8.2): once the attach is answered, the PD asks its files through the listener, and a
   PD thread unanswered for about a minute is what `sns_watchdog` kills the whole DSP for. So the bring-up after the attach takes no
   tokens and holds only for PianoPower's quiet; it has a deadline (15 s, `-rpc-deadline`); every stop after an answered attach (a step
   refused, the framing ladder exhausted, next2 failing, the deadline, a strike before the listening) first sends Linux's INIT_RELEASE
   (handle 1, method 1: what the PD gets when Linux's daemon goes away), unless the DSP shows a fatal sign, a crash reason or another
   link or charger generation (a dying DSP is never touched; the release is then withheld and said). Such a stop is a strike
   (`the file service failed after the attach`): without its files the PD has no sensors, and the SNS client and the streams stop too.
   Before an answered attach a stop stays one failure (rule 4). Whether the release itself ends the PD's waiting is inferred (Linux sends
   it at every daemon exit); s1b shows it only if the bring-up fails again.

## 8. Hardware test plan (after PianoPower level 2 is healthy; attended, RAM boot, charged battery, cable attached)

Every run with `piano-power=2` (PianoPower starts the DSP; the sensors never do), after the pack is on the ESP (section 5b).

| Level | What runs | Pass | State, effort |
|---|---|---|---|
| S1 survey | `piano-sensors=1`: health through the guard, the channel registered, the SNS port, the pack loaded and checked, the SMMU entries of 0x1003/0x1083/0x1043/0x1063 planned (loads only), the arena address checked (not mapped); no message to the DSP | `piano-sensors-dsp` up with `charger_pd 0x1fffffff`; `-guard run` 20 s after; `-channel 1` (the DSP's open answered); `-sns` announced; `-pack ok` with all four items; `-smmu` and `-arena` listed; no fatal and the charger generation unchanged for 10 min | built; 1 boot |
| S2 files | `=2`: the arena (0x9d200000 by default) mapped, the streams bypassed, attach (bank 3, then `-bank=4`), listener, every DSP call answered from the pack, writes declined | `rpc: attached to the sensors PD`, `listening frame=...` (§8.2), `first call of the DSP`, `rpc open` lines for sns_reg_config, json.lst and the registry; `-rpc` declined 0; the refused writes counted; no fatal for 15 min, charger unchanged | built; 2-4 boots |
| S3 sensor list | `=3`: + SUID lookups and attributes, no stream | `-types`: registry, accel, gyro, ambient_light, proximity, mag found with names and rates (without the file service, as SUN saw: accel none); guard `run` all along | built; 1-2 boots |
| S4 streams | `=4`: light on change -> `piano-sensors-light-target` / PianoBacklight; accel 10 Hz while the cover is detached -> `piano-sensors-edge`; 20 light off/on cycles; the streams off before a system sleep (§5c, §8.1) | lux follows a lamp, the backlight within 2 s; readings after every cycle (v1.1.1); the four edges named right (fix `-axes` if not); `ready for the sleep` before each sleep | built and host-tested; 3 boots (§8.1) |
| S5 rotation | the agent (staged and signed like the GPU plugin), the cover lock, touch rotation (the consumers' work) | each edge upright with touch at the four corners (Ubuntu's table); attached -> normal | 3-5 days, 2-3 boots |
| S6 B13 | persist's registry read at run time (ext4 reader), or the RAM overlay if S2 asked for it | readings identical with the pack's registry; at rest 9.81 +- 0.1 m/s^2 on each face; light within 10 % of Ubuntu's under one lamp | 3-5 days, 1-2 boots |

What is left: the boots of S1-S4 (one batched session, §8.1), then S5 and S6, about 1-2 weeks, and 9-14 attended boots for all six.
Fails safe: everything stops at the first guard event; a reboot returns to the tablet's normal boot.

### 8.1 The batched tablet test (S1-S4 in one attended session)

Prerequisites: PianoPower level 2 healthy on this kernel (the DSP up, `charger_pd 0x1fffffff`, no fatal), the cable to the Mac, a
charged battery, the cover, a lamp. The pack is made on the Mac from the owner's sources (§5b) and either carried by every test image
(the default: `PIANO_SENSOR_PACK`, §5b) or put on the installed ESP by the owner's maintenance session (`SENSOR_PACK=esp`).

```sh
python3 package/PianoSensors/tools/make-sensor-pack.py --registry-tar ... --rom-config ... --out out/sensors/pack.bin   # §5b
KB=$(tooling/k27/build-native-piano-keyboard.sh | tail -1)       # the cover's kext: piano-keyboard-attached, -hall
PIANO_EXTRA_KEXTS=$KB PIANO_GPU=2 PIANO_VMM_PRESENT=0 K27_KERNEL=$PIANO_K27_TREE/out/k27/kernel-k27-0063-native.macho \
  tooling/k27/build-native-image.sh
tooling/k27/build-sensor-test-images.sh           # out/ram-sensors-<step>/ (SENSOR_PACK=out/sensors/pack.bin, or =esp)
```

Each step is `fastboot boot out/ram-sensors-<step>/boot-piano-macos-ram-A-bundled.img`; its timed reset lands in ABL fastboot, and
`out/ram-sensors-dump/` reads the run's log before the next. Every image: `piano-power=2`, the GPU profile, the USB console and root
shell, the buttons, Data volatile. Run in this order and stop at the first guard strike (`piano-sensors-guard off`), DSP fatal,
charger generation change or failed pass; the tablet's normal boot is a reboot away.

| Step | Image | Do (root shell: `ioreg -rc PianoSensors -l \| grep piano-sensors`) | Pass |
|---|---|---|---|
| S1 | `s1` | wait 10 min | §8's S1 row; `-pack` `ok: ... registry ...; from the RAM image` (or `from the ESP`) |
| S2 | `s2` (`s2b4` only if the attach was refused) | wait 15 min | §8's S2 row |
| S3 | `s3` | wait 10 min | §8's S3 row; `-types` ambient_light `found 1: ... on change`, accel `found 1: ... streaming` |
| S4 | `s4` (`-als=2 -rotate=2`: no consumer needed) | a lamp off and on; off the stand: each edge up in turn, then flat; the cover attached and detached; `pmset displaysleepnow` and a key, 20 times | `-lux` follows the lamp within 2 s; `-edge` names the four edges right (else `piano-sensors-axes`, Ubuntu's table) and `none` flat; `accel: off (cover attached)` and on again; a `first sample` line after every wake; `-streams` 0 unanswered, 0 errors; no strike, the charger generation unchanged after 20 min |
| S4c | `s4c` (the consumers decide) | the lamp again; `piano-backlight-percent` (manual), then `piano-backlight-auto` 1 (setProperties, backlight.md §5) | the backlight follows the lamp within 2 s; manual: `light: off`, `-streams` `consumer: PianoBacklight is in manual mode`; automatic again: a first sample at once |
| S4z | `s4z` (k27 0062: three test sleeps) | nothing: the sleeps come every 120 s | before each sleep `streams: ready for the sleep after N ms` with N below 800 and no `ports closed`; after each wake `streams: awake` and fresh sessions; the room's level at once |

### 8.2 The first S2 (2026-11-14) and the s1b probe

The batch-2 `adsp` RAM run (level 4, PAG's framing) passed S1 and stopped S2 one step before serving. Console, in order (the file
service's `ms=` count from its start, about T0; T = the drivers' `[Ns]`):

| T | Line | Meaning |
|---|---|---|
| 25.9 | `rpc attached to the sensors PD pa=0x9d200000 bank=3 ms=25890` | attach answered 0 (bank 3, the arena reached through the bypassed stream) |
| 25.9 | (no `remotectl open failed`, no `register failed`) | open, register (no buffers, addr 0) answered 0; close unchecked |
| 25.9 | `rpc FAIL st=-6 err=-2147482610 adsp_listener init2 failed` | init2 (handle 3, method 3, sc 0x03000000, no buffers, addr 0 size 0) answered by the DSP (ctx matched) with 0x8000040e = AEE EBADPARM |
| 25.9 | `file service stopped: stage -6 error -2147482610`, `guard: backoff`, `guard: run` | the service ended; one failure for the guard; S3's lookups went on (all `found=0`) |
| 83 | `qrtr: the DSP asks the locator for "qsh_process": not answered` | the sensor PD's process asks the locator (not in PianoPower's table) |
| 84.5 | `crash reason: err_qdi.c:1215:EF:qsh_process:0x4:UTIMER_CLIENT_4:0x1000002f:sns_watchdog.c:84:` | the sensor framework's watchdog, 59 s after the attach: the whole ADSP down, the charger with it (every mca request timed out until the 900 s reset) |

Root cause of the EBADPARM (**inferred from the code**, not yet confirmed on the tablet): the init2 message differs from Linux's in
three ways, and only one of them is specific to invokes without buffers, the kind init2 is: PAG sent no payload page (`addr 0 size 0`)
where Linux always sends one (§2, §4). The two others (ctx numbering, the tid) applied equally to open and register, which were answered
0. Ubuntu's hexagonrpcd made the same calls in the same order (attach, open, register, close, init2, next2) on the same firmware, and its
init2 was answered 0. Register (a dynamically opened handle) took the page-less invoke; adsp_listener's static handle evidently does
not. The fix: Linux's page by default, the ladder for the two weaker candidates, all in the same session (§4).

Why the ADSP died (**inferred**, from the timing): the attach (or the register after it) lets the sensor PD start asking its registry
and configuration through the listener; with init2 refused nothing ever served next2, and a PD thread waited until the watchdog. With no
attach at all the DSP stayed up (PianoPower level 2 with nobody serving the channel, 215 s, 2026-11-11). Rule 7 (§7) now ends such a session with Linux's INIT_RELEASE within the
deadline and strikes the guard.

**s1b** (one RAM boot, the batch's adsp recipe with only the sensor and charging arguments, `piano-sensors=4 -als=2 -rotate=2 -settle=20`,
the defaults `-rpc-frame=1 -rpc-ladder=1 -rpc-deadline=15`): what it must show, and where.

| Question | Pass (console line, or `ioreg -rc PianoSensors -l`) |
|---|---|
| The framing | `file service: rpc-frame=1 (meta) ladder=1 deadline=15 s`; `rpc attached to the sensors PD ... frame=meta deadline=15000` |
| init2 answered 0 | `rpc listening frame=meta inits=1 ...`: H1 (the page) confirmed. `frame=meta+ctx4 inits=2` or `frame=meta+ctx4+tid inits=3` (each refusal logged as `rpc init2 frame=... answered rc=0 value=...: next frame=...`) name the other cause. `adsp_listener init2 failed` after three: none of the three |
| The file service serves | `rpc first call of the DSP handle=... sc=...`; `rpc open` lines for sns_reg_config, json.lst and the registry; `-rpc` `declined 0`, `release none`; refused writes counted, no loop |
| S3 | `-types` with `found N: name (vendor) ...` for registry, accel, gyro, mag, ambient_light, proximity (S2 working is what S3's `found=0` was missing) |
| S4 | `light: on (on change) session=1`, `light: first sample: lux=...`, `accel: on (10.000 Hz asked) session=1`, `accel: first sample: ...`; `-lux`, `-edge`, `-seq`, `-streams` set |
| No DSP fatal for 10 minutes | no `the DSP reports a FATAL error`, no `crash reason`, `-dsp` `fatal 0/0`, guard never `off`, charger generation 1 throughout |
| Charging unchanged | PianoPower's mca writes `ok (0)` with read-backs `as written`, `[Ns] mca:` lines without timeouts, the same policy steps as the batch run |
| If the bring-up fails anyway | `rpc released the sensors PD's session rc=0 value=0 ms=N after the attach` with N below 16000, then `guard: off: the file service failed after the attach`; whether the DSP then stays up past T+90 s answers whether the release ends the PD's wait |

**The s1b result** (one RAM boot, 2026-11-14 21:41, image `71357c94…`): the framing did its job and the death moved one step deeper.
The boot ended about 36 s after XNU start (`back in ABL fastboot after 55 s`, the panic, not the 900 s guard). Item by item:

| Question | Answer |
|---|---|
| The framing | **PASS** — `rpc attached to the sensors PD pa=0x9d200000 bank=3 ms=25905 frame=meta deadline=15000` (console 3823): attach answered 0 with the meta framing |
| init2 answered 0 | **PASS — H1 (the payload page) confirmed on the tablet**: `rpc listening frame=meta inits=1 ms=26032 after_attach_ms=84` (console 3825): the first init2, with the metadata page, was accepted 84 ms after the attach. No refusal, no ladder step, no `adsp_listener init2 failed` anywhere: the first S2's EBADPARM is gone |
| The file service serves | **PARTIAL — reached the serve state, died on the first call**: `rpc first call of the DSP handle=0 sc=0x20200` exists only in the mailbox (3826; the console capture had ended at the disconnect), then `Unaligned kernel data abort` (`esr: 0x96000021`, `far: 0xfffffffb5ce21021`) and `MACH Reboot` (mailbox 3827-3837). No `rpc open` for sns_reg_config, json.lst or the registry, no `declined`, no `release`: nothing was served |
| S3 | **FAIL** again: `type=registry found=0`, then accel, gyro, mag, ambient_light, proximity (mailbox 3809-3822), `sensor list settled` — consistent, the PD never got its files |
| S4 | NOT SEEN |
| No DSP fatal for 10 minutes | ran out by the host panic, not by a DSP event: `fatal 0/0` throughout, no `crash reason`, the guard only `wait`→`run`. The batch run's `sns_watchdog` death did not recur — this death was XNU-side |
| Charging unchanged | as far as it went: mca writes `ok (0)` with read-backs `as written`, the 900→1500 mA step held `5051 mV with 791 mA`, charger generation 1; the ladder beyond 1500 mA never ran |
| The release path | NOT APPLICABLE — the bring-up succeeded, so rule 7 never triggered |

The new blocker is the PD's first reverse call, one step deeper than init2. Confirmed facts: the abort line and its registers
(`far = x8+0x21`, x13−x8 = `0xf000` = `kInCapacity`, x20 = the call's sc), `MACH Reboot`; `sc=0x20200` is the remotectl-open shape
(method 2, 2 in / 2 out — exactly our own `SC(kMethodOpen,2,2)`, inferred). The exact line is inferred: the pc is in kernel/kext text,
the mailbox holds no backtrace, and the only unlogged window between `listening` and the disconnect is the read/dispatch of that first
request (a refusal would have logged `dsp opens ...` at log=2); the non-canonical `lr` fits a corrupted in-buffer length or pointer that
passed the arena bounds but not the page mapping. Whether it reproduces (deterministic vs a flipped PD message — the mailbox carried
421620 majority-corrected bytes, but the abort itself is console-timed) is unknown from one boot.

What the next image must change (inferred): before anything else, harden the first-call path — bounds-check the request blob against the
mapped arena pages, not just against `kInCapacity`, clamp and validate every buffer `n` before dereference, cap `text_of`/`same` reads to
the mapped region, and treat an out-of-range length as `SSR_EBADPARM` instead of touching it; that is the single step between "listening"
and "serving". An A/B boot arg (default keep serving) that answers the first reverse call with an error without parsing it discriminates
"the PD's message content is the trigger" vs "our parse is". Repeat the same image once unchanged if the panic reproduces. S3/S4 stay
blocked behind serving; `-settle=20` stays (the attach at T≈26 s was answered and the PD called back at once; the watchdog never fired).
The release path stays queued for a boot where the bring-up fails. Sources: the live console capture (one boot, quoted throughout); the
mailbox only for the lines the console lost, quoted where the majority-corrected copy reads cleanly.

## 9. Fallback

`piano-sensors=0` (the default): nothing runs, as today. Brightness by the keys (PianoBacklight), rotation manual (R1's System Settings
entry, if it exists for this display) or none. Ubuntu keeps its own sensor stack.

## 10. Open questions and risks

### 10.1 The API asked of PianoPower and of the other drivers

Delivered (2026-11-11): A-C as PianoPower's client contract (`package/include/PianoAdspContract.h`, `charging.md` §9: 6 GLINK slots,
8 QRTR client ports with callbacks, 2048-byte IPCRTR receive buffers, `piano_adsp_health` with the fatal bit and count, its own crash
string and the link and charger generations, `piano_adsp_quiet`, and GLINK keeping a DSP-side open that precedes the registration);
PianoKeyboard publishes `piano-keyboard-attached`. PianoSensors uses one channel and one port. D is decided: the CDSP carveout, split
in `package/include/PianoDspMemory.h` (section 5). Delivered too (2026-11-11, off by default): PianoBacklight's automatic mode reading
`piano-sensors-light-target` (`piano-backlight-auto=1`, §5a.4), PianoTouch's rotation input (`piano-touch-rotate=1`, §5a.3), and
PianoSensors' own `registerService()` in `start()` (§5a.5). Level 4 uses three of the eight QRTR ports (the list and one per stream)
and `piano_qrtr_close` (§5c). Still open: NativeGopDisplay's transform (R2 only). The original request, for the record:

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
  0x1003/0x1043; PianoWifi's and PianoDwc3's `SmmuStream.h` shows the bypass form). Now `PianoDspMemory.h`.
- The consumers' interfaces, specified in §5a with their code in `package/PianoAuto`: PianoBacklight's `PianoBacklightAmbient` and
  automatic mode (§5a.4, `AmbientBacklight.h`); PianoTouch's `PianoTouchRotation` / `piano-touch-rotation` and its transform (§5a.3,
  `TouchRotation.h`); PianoKeyboard's `piano-keyboard-attached` exists (keyboard.md §6c); NativeGopDisplay: nothing for R1, R2's
  opt-in only if R0 asks for it (§5a.6).

### 10.2 Questions

1. Does the sensor PD stay quiet for hours with nobody attached (215 s seen)? (S1, a long charging run)
2. Which bank the DSP accepts for PD 2 (3 is Linux's first session on piano, 4 the 13T's) and whether 0x1003 is in bypass, fault or
   translation when the boot hands over. (S1, S2)
3. The arena: decided (section 5, the CDSP carveout's sensors slice); whether the DSP's accesses reach it shows at the first attach. (S2)
4. Whether the PD asks for `apps_mem` pages, socinfo, DSP libraries (`/usr/lib/qcom/adsp`, empty on Ubuntu) or files outside the pack. (S2 log)
5. Whether declined writes are harmless (Ubuntu's V4 says yes) or the PD loops (then the RAM overlay). (S2)
6. One QMI client for the lookups and attributes (level 3 uses one) is enough; whether streams want one each (libssc, Android) at S4.
   Decided: one each (§5c.1); S4 shows whether the hub takes three clients from one node.
7. The rotation path (verdict R1, §5a.1): does WindowServer answer `canChangeOrientation` yes for NativeGopDisplay's panel, and does a
   rotated desktop come through its CPU copy unchanged (composited rotated) or as transposed swaps? (§5a.8 R0, no sensor needed)
8. The physical axes (left/right) (S4, S5: Ubuntu's four-edge table). macOS's sense of "90 deg" is public: clockwise, the panel's left
   edge up (CGDisplayConfiguration.h, §5a.1).
9. The front sensor's placement (bezel or under the panel: an under-display sensor would see the screen's own light). (S4, lamp off/on)
10. Jumbo reports: not requested (no TLV 0x10) to fit the router's 2048-byte buffers; whether the hub then splits or drops large ones. (S3)
11. Which rate the accelerometer's driver runs for 10 Hz (the physical configuration event says; LSM6DSO's lowest rates), and whether
    batching (`piano-sensors-accel-batch`) lowers the report rate as asked. (S4)
12. Whether the hub reports an on-change light's value right after the enable (the re-arm covers it) and how often under a steady lamp. (S4)
13. Whether the hub drops a closed port's streams (DEL_CLIENT) at once: only the sleep's fallback and a strike close ports. (S4z)

### 10.3 Level 4's risks

1. The light's consumer is read from PianoBacklight's boot arguments and properties: a level the user chose before the first target
   looks like "no target yet" (its published `piano-backlight-auto` is false for both), so the light stream keeps running, its targets
   refused, until PianoBacklight publishes its last target; bounded, and shown as the consumer in `piano-sensors-streams`.
2. Without PianoKeyboard the cover reads detached and the lid open: the accelerometer streams whenever the agent runs and the display
   is on; the batched test links PianoKeyboard.
3. Before NativeGopDisplay's first power change `piano-display-power` is absent and the display counts as on (as it is at boot).
4. The sleep's fallback closes a port (DEL_CLIENT) when a disable is not answered within 800 ms; with PianoPower asking for quiet it
   does not, and the stream stays on through the sleep (logged): the hub's reports then wait in GLINK until the wake.
5. The edge's axes and the light's placement are inferred (5a.9 risk 7); the rates the hub chooses are logged, not checked.

## 11. Licence note

PianoSensors is BSD-2-Clause-Patent. PaganiSensors (BSD-2-Clause-Patent) is reused with `UPSTREAM` records. Linux's fastrpc driver and
the SM8750 devicetree (GPL-2.0), hexagonrpc and the Ubuntu project's patches to it (LGPL-3.0 / GPL-3.0), libssc (GPL-3.0), iio-sensor-proxy
(GPL-3.0) and libqmi (LGPL-2.1) are specification only: behaviour, numbers and layouts as facts, no code or text copied. qualcomm/fastrpc
(BSD-3-Clause) was read for its IDL's numbers only. No Qualcomm, ST, Sensortek or Xiaomi binary is reverse engineered. The DSP firmware,
the vendor/ODM sensor configuration and this unit's persist registry are data read at run time from the owner's own tablet (or a pack
under the ignored `out/`), never committed and never another unit's. PianoAuto (BSD-2-Clause-Patent) is this repository's own code; of
Apple's it uses public SDK headers, and for the one private call (System Settings' rotation setter) class and selector names with their
runtime type encodings, plus exported symbol names that are only cited: no Apple binary was disassembled or decompiled
(`package/PianoAuto/UPSTREAM`). The msm DPU catalog (GPL-2.0) gave the pipes' rotation facts only. Level 4's requests, events and
client model come from libssc (GPL-3.0) and Android's sensor behaviour (the reporting modes, one client per sensor) as specification
only, Linux's qrtr and QMI interface (GPL-2.0) for what a gone client means; the RAM image's carrier follows UniXNU's addon layout
(`scripts/make-gpu-addon.py`, DarwinBoot's `GpuAddon` slot) as a format; the kext inflates it with libkern's own zlib.
