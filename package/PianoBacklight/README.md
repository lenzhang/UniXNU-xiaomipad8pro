PianoBacklight: the panel backlight of the Xiaomi Pad 8 Pro (two Kinetic KTZ8866 at I2C 0x11, on the I2C hub's SE3 pins gpio70/71
and QUP2 SE0's pins gpio0/1, driven in software) for native macOS 27. Inert without the boot argument piano-backlight (1: read-only
survey; 2: control: a moderate boot level, backlight off on display sleep and on again on wake, brightness steps, optionally off
while the cover is closed). The design and boot arguments are at the top of PianoBacklight.cpp, the register facts and the state
machine in Ktz8866.h, the interface for other drivers in PianoBacklightClient.h; the page is docs/drivers/backlight.md; provenance
in UPSTREAM. Build: tooling/k27/build-native-piano-backlight.sh (host tests, then the objects); it is in configs/k27-native-kexts.txt.

| File | What |
|---|---|
| `PianoBacklight.cpp` | the IOService on IOPlatformExpertDevice: pins, the two software buses, the thread, the callouts, the properties |
| `Ktz8866.h` | portable: registers, survey decoding, brightness steps, the log limit, the controller (what is written when) |
| `BitI2c.h` | portable: the software I2C master (PianoKeyboard's, copied) |
| `PianoBacklightClient.h` | header only: the function names other drivers call, the brightness key watch (`piano-backlight-keys=1`) |
| `tests/backlight_host.cpp` | host tests (bit-level KTZ8866 model, register sequences, sleep/wake, lid, failures, keys) |

Status (2026-11-11): built and host-tested, not run on the tablet.
