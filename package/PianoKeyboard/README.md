PianoKeyboard: the Xiaomi Pad 8 Pro keyboard cover (Nanosic WN8030 over I2C on gpio56/57) as a keyboard and a pointer for native
macOS 27. Inert without the boot argument piano-keyboard (1: bring-up and raw reports in the log; 2: HID devices for macOS); the
design, steps and boot arguments are at the top of PianoKeyboard.cpp; provenance in UPSTREAM. Build: tooling/k27/build-native-piano-keyboard.sh
(host tests, the pinned MCU image staged into out/, the objects, the resolved descriptor path printed last).

Not yet run on the tablet (2026-11-11): nothing here is measured yet.
