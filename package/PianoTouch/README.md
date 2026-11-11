PianoTouch: the Xiaomi Pad 8 Pro touch screen (Novatek NT36532E, host-processed THP, on QUP1 SE2 = spi@a88000, IRQ gpio162) for
native macOS 27, as a single-touch absolute pointer (a tap clicks, a drag drags). Inert without the boot argument piano-touch (1: bus,
chip ID, firmware download, frame statistics and contacts in the log, nothing for macOS; 2: the same and the HID tablet pointer). The
design, steps and boot arguments are at the top of PianoTouch.cpp and in docs/drivers/touch.md; provenance in UPSTREAM. Build:
tooling/k27/build-native-piano-touch.sh (host tests, the pinned panel images staged into out/, the unit's private touchscreen
calibration turned into an ignored header when PIANO_TOUCH_CALIBRATION names it, the objects, the resolved descriptor).

Not yet run on the tablet (2026-11-11): nothing here is measured yet.
