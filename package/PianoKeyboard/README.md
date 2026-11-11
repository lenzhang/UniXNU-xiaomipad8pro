PianoKeyboard: the Xiaomi Pad 8 Pro keyboard cover (Nanosic WN8030 over I2C on gpio56/57) as a keyboard and a pointer for native
macOS 27, and the folio's hall sensor (gpio80, read only). Inert without the boot argument piano-keyboard (1: bring-up and raw
reports in the log; 2: HID devices for macOS); the design, steps and boot arguments are at the top of PianoKeyboard.cpp, the page is
docs/drivers/keyboard.md; provenance in UPSTREAM. Build: tooling/k27/build-native-piano-keyboard.sh (host tests, the pinned MCU image
staged into out/, the objects, the resolved descriptor path printed last; PIANO_EXTRA_KEXTS for build-native-image.sh).

Status (2026-11-11): level 1 ran on the tablet once (logs/usb-console/20261111-084701-port0.txt, 240 s): rails voted, boot ROM id
0xc8, the MCU image loaded (101 blocks in 7 s) and booted, frames read on gpio97 edges, the cover attached ("cover ATTACHED (attach
message)"), keyboard power / Caps LED / touchpad-on commands acknowledged, the 12 s keyboard-power keepalive running, the
authentication request left unanswered. Nobody typed or touched the touchpad in that run, so no key or touchpad report has been
seen yet; level 2 (macOS HID devices), the hall sensor and the DRV-2 lock have not run. Tonight's test: docs/drivers/keyboard.md §8.
