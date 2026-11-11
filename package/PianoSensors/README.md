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
  curve and the rotation (`SensorsPolicy.c`).

**Status: offline, host-tested code only.** No kext, no boot argument, nothing in the kext list; nothing has met the DSP. Design, facts,
the bring-up ladder and the API asked of PianoPower: `docs/drivers/sensors.md`.

| File | What |
|---|---|
| `SensorsPb.c/.h` | protobuf wire format (PaganiSensors, unchanged) |
| `SensorsSsc.c/.h` | QMI frames and the SNS messages of service 400 (PaganiSensors, unchanged) |
| `SensorsRpc.c/.h` | the FastRPC reverse file service, portable over an ops table (PaganiSensors, unchanged) |
| `SensorsPack.c/.h`, `tools/make-sensor-pack.py` | the read-only file pack and its builder (builder derived: piano's archives, no serials, never in a repository) |
| `SensorsFixed.c/.h`, `SensorsOrient.c/.h` | integer float conversions, axis maps, the orientation state machine (PaganiSensors, unchanged) |
| `SensorsGuard.c/.h` | new: when the client may act at all (charger first, tokens, backoff, one strike on any DSP trouble) |
| `SensorsPolicy.c/.h` | new: piano's facts, the light curve with smoothing and hysteresis, panel edge and rotation with the cover lock |
| `tests/` | `run-host-tests.py`: PaganiSensors' wire/fixed/orient/rpc tests, the pack tool, `piano.c` (guard, policy, cross-checks against libqmi, libssc, Linux fastrpc.c and qualcomm/fastrpc), the kernel's no-FP/no-libc compile |

Run: `python3 package/PianoSensors/tests/run-host-tests.py` (ASan/UBSan, synthetic inputs only). The pack holds a unit's calibration:
build it only from the owner's own tablet, under the ignored `out/` or `private/`; it is never committed. Sources and licences: `UPSTREAM`,
`LICENSE` (BSD-2-Clause-Patent).
