# PianoAuto

The macOS-side consumers of piano's sensors (PianoSensors, `docs/drivers/sensors.md` section 5a): automatic display rotation with
the keyboard cover's lock and the touch screen following it, and automatic brightness from the ambient light sensor with streams
that resume after every off/on. They are independent of the sensor transport (which waits on PianoPower's client API) and are tested
on the host now.

**Status: offline, host-tested code only (2026-11-11).** PianoAuto itself is not a kext and nothing has run on the tablet. The headers
are taken in by PianoTouch (`piano-touch-rotate=1`) and PianoBacklight (`piano-backlight-auto=1`), both linked and off by default;
`AutoStream`/`AutoLight` wait for PianoSensors' kext at its level S4. Interfaces: `sensors.md` section 5a.

| File | What | Used by |
|---|---|---|
| `TouchRotation.h` | the touch coordinates under a display rotation (CGDisplayRotation's degrees), latched at touch-down | PianoTouch (`piano-touch-rotate=1`) |
| `AutoRotate.c/.h` | the rotation agent's policy over PianoSensors' `ps_rotation_step`: the cover lock and detach hold, reading back, backoff, rotations by hand, touch | piano-rotated |
| `AutoStream.c/.h` | one sensor stream's on/off lifecycle: one request outstanding, no nested close, first events kept, re-arm, timeouts (Ubuntu's v1.1.1 fix as a rule) | PianoSensors' kext (S4) |
| `AutoLight.c/.h` | the ambient light chain: the stream with the display and the lid, `ps_als_step`'s curve and smoothing, a fresh start after each off/on, targets for PianoBacklight | PianoSensors' kext (S4) |
| `AmbientBacklight.h` | PianoBacklight's automatic mode: targets to codes, the brightness keys as a kept offset on PianoBacklight's own step ladder, manual/automatic | PianoBacklight (`piano-backlight-auto=1`) |
| `daemon/piano-rotated.m` | the launchd agent (login window and user sessions): PianoSensors' edge and PianoKeyboard's cover in, WindowServer's own rotation out (MonitorPanel's setter by name, read back with the public `CGDisplayRotation`), PianoTouch told; inert without the boot argument `piano-rotate` | native macOS |
| `daemon/com.unixnu.piano-rotated.plist`, `daemon/install-piano-rotated.sh` | its global LaunchAgent job and the on-tablet install (root) | |
| `tests/` | `run-host-tests.py`: `auto_host.c` with `stream_tests.h`, `ambient_host.cpp` (ASan/UBSan), the agent's compile, the kernel's no-FP/no-libc compile | |

Run: `python3 package/PianoAuto/tests/run-host-tests.py`. Build the agent: `tooling/k27/build-piano-rotated.sh` (the tests, then
`out/piano-rotated/`). Sources and licences: `UPSTREAM`, `LICENSE` (BSD-2-Clause-Patent).
