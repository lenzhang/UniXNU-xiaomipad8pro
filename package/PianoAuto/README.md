# PianoAuto

The macOS-side consumers of piano's sensors (PianoSensors, `docs/drivers/sensors.md` section 5a): automatic display rotation with
the keyboard cover's lock and the touch screen following it. They are independent of the sensor transport (which waits on
PianoPower's client API) and are tested on the host now.

**Status: offline, host-tested code only (2026-11-11).** Nothing is in the kext list and nothing has run on the tablet. The drivers
that take these pieces in (PianoTouch) are owned elsewhere and are not edited here: their interfaces are specified in
`sensors.md` section 5a.

| File | What | Used by |
|---|---|---|
| `TouchRotation.h` | the touch coordinates under a display rotation (CGDisplayRotation's degrees), latched at touch-down | PianoTouch (to add) |
| `AutoRotate.c/.h` | the rotation agent's policy over PianoSensors' `ps_rotation_step`: the cover lock and detach hold, reading back, backoff, rotations by hand, touch | piano-rotated |
| `tests/` | `run-host-tests.py`: `auto_host.c` (ASan/UBSan), the kernel's no-FP/no-libc compile | |

Run: `python3 package/PianoAuto/tests/run-host-tests.py`. Sources and licences: `UPSTREAM`, `LICENSE` (BSD-2-Clause-Patent).
