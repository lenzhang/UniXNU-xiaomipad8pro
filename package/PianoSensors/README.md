# PianoSensors

Piano's sensors for native macOS 27: ambient light (automatic brightness through PianoBacklight), the accelerometer (display rotation,
the normal landscape while the keyboard cover is attached), gyroscope, proximity and magnetometer reachable but unused. Every sensor
hangs off the audio DSP's sensor hub (the SSC in `msm/adsp/sensor_pd`), so this is three things on PianoPower's running DSP:

- the FastRPC reverse file service the sensor PD needs before it has any sensor (on Ubuntu hexagonrpcd, on Android sensors.qti):
  `fastrpcglink-apps-dsp`, attach to the sensors PD, `adsp_default_listener`, then `adsp_listener_next2` serving read-only `apps_std`
  calls from a pack of this tablet's own registry and configuration (`SensorsRpc.c`, `SensorsPack.c`, `tools/make-sensor-pack.py`);
- a QMI client of the hub's sensor client service 400 over PianoPower's router: SUID lookups, attributes, streams (`SensorsSsc.c`,
  `SensorsPb.c`);
- the policies: a safety governor that keeps the shared DSP (and its charger firmware) out of harm's way (`SensorsGuard.c`), the light
  curve and the rotation (`SensorsPolicy.c`), the streams' off/on lifecycle (PianoAuto's `AutoStream.c`/`AutoLight.c`, `SensorsStream.c`).

**Status: levels 1-4 built and linked into the KC, not run.** `piano-sensors=1` a read-only survey (PianoPower's health through the
guard, the FastRPC channel, the SNS service, the pack on the ESP, the SMMU entries, the arena address), `=2` the file service serving the
pack (its arena by default the CDSP carveout's sensors slice, `package/include/PianoDspMemory.h`), `=3` the SNS sensor list, `=4` the
streams: the ambient light on change and the accelerometer at 10 Hz, each on a QMI client of its own and only while a consumer needs it,
published as `piano-sensors-lux`, `-light-target`, `-edge`, `-seq`; without `piano-sensors` the driver declines in probe(). Design,
facts, the ladder, the properties and the pack's commands: `docs/drivers/sensors.md` (level 4: section 5c).

| File | What |
|---|---|
| `SensorsPb.c/.h` | protobuf wire format (PaganiSensors, unchanged) |
| `SensorsSsc.c/.h` | QMI frames and the SNS messages of service 400 (PaganiSensors, unchanged) |
| `SensorsRpc.c/.h` | the FastRPC reverse file service, portable over an ops table (PaganiSensors; derived: the guard's admit hook) |
| `SensorsPack.c/.h`, `tools/make-sensor-pack.py` | the read-only file pack and its builder (builder derived: piano's archives, no serials, never in a repository) |
| `SensorsFixed.c/.h`, `SensorsOrient.c/.h` | integer float conversions, axis maps, the orientation state machine (PaganiSensors, unchanged) |
| `PianoSensors.cpp` | the kext: two threads, PianoPower's contract (channel, QRTR port, health, quiet), the pack, the arena, the SMMU streams, the properties |
| `SensorsGuard.c/.h`, `SensorsHealth.h` | new: when the client may act at all (charger first, tokens, backoff, one strike on any DSP trouble); PianoPower's report as its input |
| `SensorsClient.c/.h` | new: level 3's SNS client on one QRTR port (SUID lookups, attributes) |
| `SensorsStream.c/.h` | new: level 4's streams, each on its own QRTR port: the requests (514 / 513 / 10), the reports (1025, 768, 130), PianoAuto's AutoStream rules and AutoLight chain, the outputs, the sleep and strike teardown, a rate-limited log |
| `SensorsLoad.c/.h`, `FatVolume.c/.h` | new: the pack read whole from the ESP and checked; PianoPower's FAT reader (copy) |
| `SensorsAddon.c/.h`, `tools/sensor_addon.py` | new: the pack carried by a RAM test image (`PIANO_SENSOR_PACK`): UniXNU's boot addon container in DarwinBoot's `GpuAddon` slot, its writer and its checked reader (sensors.md §5b) |
| `SmmuStream.h` | PianoDwc3's bypass-bank planner (copy) for the sensors PD's four stream ids |
| `SensorsPolicy.c/.h` | new: piano's facts, the light curve with smoothing and hysteresis, panel edge and rotation with the cover lock |
| `tests/` | `run-host-tests.py`: PaganiSensors' wire/fixed/orient/rpc tests, the pack tool, `piano.c` (guard, policy, cross-checks against libqmi, libssc, Linux fastrpc.c and qualcomm/fastrpc), `streams.c` (level 4 against a simulated hub sending the service's message shapes), `addon.c` (the RAM image's carrier), the kernel's no-FP/no-libc compile |

Build: `tooling/k27/build-native-piano-sensors.sh` (host tests, copy checks, objects, import check). Tests alone:
`python3 package/PianoSensors/tests/run-host-tests.py` (ASan/UBSan, synthetic inputs only; the loader test needs mtools). The pack holds a unit's calibration:
build it only from the owner's own tablet, under the ignored `out/` or `private/`; it is never committed. A RAM test image may carry
it (`PIANO_SENSOR_PACK`, `tooling/k27/build-sensor-test-images.sh`); such an image is as private as the pack. Sources and licences: `UPSTREAM`,
`LICENSE` (BSD-2-Clause-Patent).
