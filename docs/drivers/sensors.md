# Sensors: ambient light, accelerometer, gyroscope, proximity through the ADSP's sensor hub: PianoSensors

Status: levels 1-3 built, 2026-11-11 (backlog B3 and B13). `package/PianoSensors` is a k27 native kext on PianoPower's client
contract (`package/include/PianoAdspContract.h`, `charging.md` §9): level 1 a read-only survey, level 2 the FastRPC reverse file service
serving the owner's sensor pack from the ESP, level 3 the SNS client's sensor list. Host-tested (`tests/run-host-tests.py`: ASan/UBSan,
synthetic inputs only, a simulated DSP and hub, the pack on an mtools FAT32 image, the kernel's no-FP/no-libc compile), compiled with the
kernel's flags and **linked into the KC** (`configs/k27-native-kexts.txt`, after PianoPower and NativeUfs; the 2026-11-11 link with main's list:
PianoSensors 433 pointers among 20 native kexts), inert without `piano-sensors`. **Not run on hardware.** Template: `README.md`. The macOS-side consumers (the rotation agent `piano-rotated`, PianoTouch's transform, PianoBacklight's
automatic mode, the streams' off/on lifecycle) are `package/PianoAuto`, host-tested (§5a).

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
their tests. Derived: the pack builder (piano's ODM archive, a soc0 allowlist, never written into a repository) and `SensorsRpc` (an
`admit` hook: a call the guard declines is answered at once with `SSR_EUNSUPPORTED`, counted as `declined`). PAG's manager
(`SensorsCore`: streams, retries, iOS units) stays the template of level 4's streams. New: `SensorsGuard` and `SensorsHealth.h`
(section 7), `SensorsPolicy` (section 5), `SensorsClient` (level 3: one connection, lookups then attributes, the guard asked for each
request), `SensorsLoad` (the pack from the ESP), `PianoSensors.cpp` (the kext), `tests/piano.c` and `tests/load.c`.

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
  |-- GLINK "fastrpcglink-apps-dsp" (contract) ----------> file service: attach PD 2, listener, apps_std from the pack (read only)
  |-- one QRTR client port following service 400 --------> SNS client: lookups, attributes (level 3); streams (level 4)
  '-- piano_adsp_health / piano_adsp_quiet --------------> SensorsGuard (section 7) gates both
PianoSensors : IOService on IOPlatformExpertDevice (not HID: no macOS consumer reads sensor HID services for this display)
  |-- ambient_light (514, on change; AutoLight) -> SensorsPolicy curve -> PianoBacklight "PianoBacklightAmbient" (requested, §5a.4)
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
| `piano-sensors` | number | all levels | the level running (1..3) |
| `piano-sensors-guard` / `-guard-reason` | string | level 1+ | `wait`, `run`, `backoff`, `off` / `none` or why it is off (`sg_reason_name`) |
| `piano-sensors-dsp` | string | level 1+ | PianoPower's health: stage, up, fatal/reports, link and charger generations, quiet, crash reason |
| `piano-sensors-channel` / `-channel-ups` | number | level 1+ | 1 while `fastrpcglink-apps-dsp` is up / how often it came up |
| `piano-sensors-sns` | string | level 1+ | where service 400 is announced (node, port, instance, generation), or not announced |
| `piano-sensors-pack` | string | level 1+ | the pack's verdict: `ok: N entries, B bytes;` and which of sns_reg_config, json.lst, registry, soc0 it holds, or the error |
| `piano-sensors-smmu`, `-arena` | string | level 1+ | the four streams' entries (planned at 1, set at 2); the arena's address and verdict |
| `piano-sensors-rpc` | string | level 1+ | the file service: stage, attaches, calls, opens, missing, reads, refused (writes), declined (guard), bytes, first missing path |
| `piano-sensors-types` | dictionary | level 3 | data type (`registry`, `accel`, `gyro`, `mag`, `ambient_light`, `proximity`) -> `pending`, `none`, `no answer`, or `found N: name (vendor), available A, streaming/on change, rates L-H Hz` |
| `piano-sensors-lux` | number | level 4 (reserved) | the front light sensor's last reading, integer lux |
| `piano-sensors-light-target` | number | level 4 (reserved) | the backlight target, permille 0..1000 (`ps_als_step`) |
| `piano-sensors-edge` | string | level 4 (reserved) | the panel edge that is up: `top`, `left`, `bottom`, `right`, `none` (`ps_edge_name` of `ps_edge_of`, after SensorsOrient's hold) |
| `piano-sensors-seq` | number | level 4 (reserved) | moves with every change of the three above |

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
Mac with macOS 26.6. Nothing ran on the tablet. PianoTouch, PianoBacklight, PianoKeyboard and NativeGopDisplay are not edited: their
owners add the interfaces below. Independent of the sensor transport: everything but §5a.5 works with a test edge source today.

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
PianoSensors  piano-sensors-edge (0..3; 255 none) + kIOMessageServicePropertyChange --+
PianoKeyboard piano-keyboard-attached (0/1)        + kIOMessageServicePropertyChange --+--> AutoRotate (ps_rotation_step inside)
the user      defaults com.unixnu.piano-rotated Locked; notifyutil -p ...prefs     --+        |
WindowServer  CGDisplayRotation (read back) + the display reconfiguration callback   --+        v
              <-- -[MPDisplay setOrientation:] (by name, encodings checked first)    <-- rotate (level 2)
PianoTouch    <-- piano-touch-rotation (setProperties): the rotation shown            <-- touch (level 3)
```

A global LaunchAgent (`daemon/com.unixnu.piano-rotated.plist`: LoginWindow and Aqua sessions, since WindowServer's display services are
per session; a system daemon would not reach them), event driven (no polling; a timer only while a settle, hold or read-back is due),
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

### 5a.3 PianoTouch: the interface asked (`TouchRotation.h`; its owner adds it)

- State `PatRotation fRotation` (`pat_init` in `start`). Inputs, both only recording (`pat_request`), never waiting:
  `callPlatformFunction("PianoTouchRotation", false, (void *)(uintptr_t)degrees, (void *)source, 0, 0)` for kernel callers, and
  `setProperties({"piano-touch-rotation": 0|90|180|270})` for the agent, allowed for root (`kIOClientPrivilegeAdministrator`) or the
  console user (`kIOClientPrivilegeConsoleUser`: the agent runs as the logged-in user); other values `kIOReturnBadArgument`, other
  callers `kIOReturnNotPrivileged`. Published: `piano-touch-rotation` (the rotation in use).
- In `contact()`/`send()`: `r = pat_latch(&fRotation, e == TipEvent::down)`, `p = pat_rotate(r, x, y, kPanelW, kPanelH)`,
  `tabletReport(tip, p.x, p.y, p.width, p.height, report)`. The degrees are CGDisplayRotation's (the agent sends what it read back); a
  change takes effect at the next touch-down, never mid-drag. At 0 the report is the same as today's. One log line per change.

### 5a.4 Automatic brightness: PianoBacklight's interface asked (`AmbientBacklight.h`; its owner adds it)

- Boot argument `piano-backlight-auto=1` (default 0: targets refused with `kIOReturnUnsupported`, nothing changes).
- `callPlatformFunction("PianoBacklightAmbient", false, (void *)(intptr_t)permille, (void *)source, (void *)(uintptr_t)lux, 0)`:
  permille of the code range (1000 = 2047), the light policy's output; -1 leaves automatic mode with the code as it is. Records the
  newest and wakes the thread like `PianoBacklightStep`; `kIOReturnNotReady` before its lock exists. `PianoBacklightClient.h` gains the
  name and an `ambient()` helper.
- `Controller` takes `pianoauto::Ambient` (three calls, the header's comment): a target applies `code()`; a step in automatic mode
  shifts the curve by one of its own steps (x 2^(1/3) linear, 1/16 exponential) and the offset stays for the boot, a press always a
  visible step unless at an end; `piano-backlight-percent` (administrator) switches to manual; `piano-backlight-auto` 0/1 as a
  property leaves or re-enters (the last target at once, the offset kept). Written only on a change, nothing while dark (the newest
  code lights the wake).
- Published: `piano-backlight-auto` (automatic now), `piano-backlight-offset`, `piano-backlight-ambient-permille`, `-ambient-lux`.
- Log: ambient writes folded into one line per 10 s (`ambient: N targets, lux A..B, permille P..Q, offset K, code X -> Y`; failed
  accesses still logged at once); key steps in automatic mode `step +1 in automatic mode: offset 0 -> 1, code 757 -> 954`.

### 5a.5 PianoSensors' side (its kext at S4: `AutoStream.h`, `AutoLight.h`)

- Ambient light: one `PalLight`; conditions: `piano-sensors-als` and PianoBacklight taking targets, NativeGopDisplay's
  `piano-display-power`, PianoKeyboard's `piano-keyboard-hall` (unknown counts as open). Each `pas_action` goes out only with
  `sg_may_request`'s token (enable = 514 on change, disable = 10, on the light's own QMI client), answers matched by QMI transaction id
  to `pas_response`, indications to `pal_event` (data[0]), a timer at `pal_deadline` to `pal_tick`; each target to
  `PianoBacklightAmbient`; `PAS_FAILED` sends -1 once. The rules make readings resume after every off/on (Ubuntu v1.1.1): one request
  outstanding, no nested close, the first event kept even before the enable's answer, a re-arm when it never comes, a fresh start
  from it (no fade from the level before the display slept).
- Accelerometer: one `PasStream` (513 at 10 Hz), wanted with the display on, the cover detached (`piano-keyboard-attached` 0 or
  absent) and `piano-sensors-rotate`; samples through `ps_accel_sample` (a `PAS_EV_FIRST` restarts `sno_init`); on each new state
  `piano-sensors-edge` = `ps_edge_of` (0..3, 255 for none: flat, face up/down, unknown, stream off), `piano-sensors-edge-changes`, and
  `messageClients(kIOMessageServicePropertyChange)` as PianoKeyboard does.
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

Staging (the A830 path's seed plus the agent; `PIANO_SEED`/`PIANO_TRUST` make build-native-image.sh take it):
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
| R2 | PianoTouch's interface, `piano-rotate=3 piano-touch=2`: at each rotation the four corners and the centre | the pointer under the finger (about 10 px); a drag never jumps at a rotation |
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

## 6. Boot args

| Argument | Default | Values |
|---|---|---|
| `piano-sensors=N` | 0 | 0 nothing (probe declines); 1 read-only survey; 2 + the FastRPC file service; 3 + the SNS sensor list (built); 4 + streams and the consumers (next) |
| `piano-sensors-rpc-pa=ADDR` | 0x9d200000 | the arena's physical address (8 MiB; default the CDSP carveout's sensors slice, `PianoDspMemory.h`; another address only outside every known user, `ps_arena_check`; 0: no file service); level 2 runs no file service without one |
| `piano-sensors-bank=N` | 3 | the context bank (3..8); its streams 0x1000+N, +0x80, +0x40, +0x60 |
| `piano-sensors-smmu=0` | 1 | leave the SMMU alone at level 2 (the streams must bypass already) |
| `piano-sensors-settle=S` | 20 | seconds of the charger's newest generation before the first request |
| `piano-sensors-nopack=1` | 0 | level 2 with an empty pack (every open fails and is logged) |
| `piano-sensors-channel=0` | 1 | level 1 does not register the FastRPC channel |
| `piano-sensors-delay=S`, `-log=0..2` | 0, 1 | wait first; log volume (2: every SNS line) |
| `piano-sensors-axes=a..` (level 4) | `a+y+x+z` | the accelerometer map (SensorsFixed text), for the four-edge test |

The consumers' own arguments: `piano-rotate*` (the agent, §5a.2) and `piano-backlight-auto=1` (PianoBacklight, §5a.4).

## 7. Kernel component, and the safety story

Component `PianoSensors` (`com.unixnu.driver.PianoSensors`, `tooling/k27/kext/PianoSensors.json` naming `com.unixnu.driver.PianoPower`
and `com.unixnu.driver.NativeUfs` as libraries), listed after both in `configs/k27-native-kexts.txt`; built by
`tooling/k27/build-native-piano-sensors.sh` (host tests, the copies of FatVolume and SmmuStream checked against their originals, the
objects, the import check: 335 kernel imports, 8 from the libraries). Two threads (`piano-sensors`: health, guard, SNS client,
properties; `piano-sensors-rpc`: the file service); PianoPower's callbacks only record and wake them. Rules (`SensorsGuard.c` and
`SensorsHealth.h`, host-tested):

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

## 8. Hardware test plan (after PianoPower level 2 is healthy; attended, RAM boot, charged battery, cable attached)

Every run with `piano-power=2` (PianoPower starts the DSP; the sensors never do), after the pack is on the ESP (section 5b).

| Level | What runs | Pass | State, effort |
|---|---|---|---|
| S1 survey | `piano-sensors=1`: health through the guard, the channel registered, the SNS port, the pack loaded and checked, the SMMU entries of 0x1003/0x1083/0x1043/0x1063 planned (loads only), the arena address checked (not mapped); no message to the DSP | `piano-sensors-dsp` up with `charger_pd 0x1fffffff`; `-guard run` 20 s after; `-channel 1` (the DSP's open answered); `-sns` announced; `-pack ok` with all four items; `-smmu` and `-arena` listed; no fatal and the charger generation unchanged for 10 min | built; 1 boot |
| S2 files | `=2`: the arena (0x9d200000 by default) mapped, the streams bypassed, attach (bank 3, then `-bank=4`), listener, every DSP call answered from the pack, writes declined | `rpc: attached to the sensors PD`, `listening`, `first call of the DSP`, `rpc open` lines for sns_reg_config, json.lst and the registry; `-rpc` declined 0; the refused writes counted; no fatal for 15 min, charger unchanged | built; 2-4 boots |
| S3 sensor list | `=3`: + SUID lookups and attributes, no stream | `-types`: registry, accel, gyro, ambient_light, proximity, mag found with names and rates (without the file service, as SUN saw: accel none); guard `run` all along | built; 1-2 boots |
| S4 streams | `=4`: light on change -> `piano-sensors-light-target` / PianoBacklight; accel 10 Hz while the cover is detached -> `piano-sensors-edge`; 20 light off/on cycles | lux follows a lamp, the backlight within 2 s; readings after every cycle (v1.1.1); the four edges named right (fix `-axes` if not) | next: 2-3 days, 2 boots |
| S5 rotation | the agent (staged and signed like the GPU plugin), the cover lock, touch rotation (the consumers' work) | each edge upright with touch at the four corners (Ubuntu's table); attached -> normal | 3-5 days, 2-3 boots |
| S6 B13 | persist's registry read at run time (ext4 reader), or the RAM overlay if S2 asked for it | readings identical with the pack's registry; at rest 9.81 +- 0.1 m/s^2 on each face; light within 10 % of Ubuntu's under one lamp | 3-5 days, 1-2 boots |

What is left: S4-S6, about 2-3 weeks, and 9-14 attended boots for all six. Fails safe: everything stops at the first guard event; a
reboot returns to Ubuntu.

## 9. Fallback

`piano-sensors=0` (the default): nothing runs, as today. Brightness by the keys (PianoBacklight), rotation manual (R1's System Settings
entry, if it exists for this display) or none. Ubuntu keeps its own sensor stack.

## 10. Open questions and risks

### 10.1 The API asked of PianoPower and of the other drivers

Delivered (2026-11-11): A-C as PianoPower's client contract (`package/include/PianoAdspContract.h`, `charging.md` §9: 6 GLINK slots,
8 QRTR client ports with callbacks, 2048-byte IPCRTR receive buffers, `piano_adsp_health` with the fatal bit and count, its own crash
string and the link and charger generations, `piano_adsp_quiet`, and GLINK keeping a DSP-side open that precedes the registration);
PianoKeyboard publishes `piano-keyboard-attached`. PianoSensors uses one channel and one port. D is decided: the CDSP carveout, split
in `package/include/PianoDspMemory.h` (section 5). Still open: PianoBacklight's and PianoTouch's inputs (for level 4), NativeGopDisplay's
transform (R2 only). The original request, for the record:

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
7. The rotation path (verdict R1, §5a.1): does WindowServer answer `canChangeOrientation` yes for NativeGopDisplay's panel, and does a
   rotated desktop come through its CPU copy unchanged (composited rotated) or as transposed swaps? (§5a.8 R0, no sensor needed)
8. The physical axes (left/right) (S4, S5: Ubuntu's four-edge table). macOS's sense of "90 deg" is public: clockwise, the panel's left
   edge up (CGDisplayConfiguration.h, §5a.1).
9. The front sensor's placement (bezel or under the panel: an under-display sensor would see the screen's own light). (S4, lamp off/on)
10. Jumbo reports: not requested (no TLV 0x10) to fit the router's 2048-byte buffers; whether the hub then splits or drops large ones. (S3)

## 11. Licence note

PianoSensors is BSD-2-Clause-Patent. PaganiSensors (BSD-2-Clause-Patent) is reused with `UPSTREAM` records. Linux's fastrpc driver and
the SM8750 devicetree (GPL-2.0), hexagonrpc and the Ubuntu project's patches to it (LGPL-3.0 / GPL-3.0), libssc (GPL-3.0), iio-sensor-proxy
(GPL-3.0) and libqmi (LGPL-2.1) are specification only: behaviour, numbers and layouts as facts, no code or text copied. qualcomm/fastrpc
(BSD-3-Clause) was read for its IDL's numbers only. No Qualcomm, ST, Sensortek or Xiaomi binary is reverse engineered. The DSP firmware,
the vendor/ODM sensor configuration and this unit's persist registry are data read at run time from the owner's own tablet (or a pack
under the ignored `out/`), never committed and never another unit's. PianoAuto (BSD-2-Clause-Patent) is this repository's own code; of
Apple's it uses public SDK headers, and for the one private call (System Settings' rotation setter) class and selector names with their
runtime type encodings, plus exported symbol names that are only cited: no Apple binary was disassembled or decompiled
(`package/PianoAuto/UPSTREAM`). The msm DPU catalog (GPL-2.0) gave the pipes' rotation facts only.
