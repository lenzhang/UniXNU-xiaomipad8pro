PianoTouch: the Xiaomi Pad 8 Pro touch screen (Novatek NT36532E, host-processed THP, on QUP1 SE2 = spi@a88000, IRQ gpio162) for
native macOS 27. Inert without the boot argument piano-touch (1: bus, chip ID, firmware download, frame statistics, contacts and
gestures in the log, nothing for macOS; 2: the same and the HID pointer). macOS has no touch screen interface, so level 2 is what
it understands from a HID pointer: tap = click, hold = right click, drag = left drag, two-finger scroll, two-finger tap = right
click, palms rejected, a pinch recognised (sent as Command or Control + wheel only on request). The design, steps and boot
arguments are at the top of PianoTouch.cpp and in docs/drivers/touch.md (§14 the gestures, §15 the attended test list, §16 the
power states); provenance in UPSTREAM.

Files: Nt36532.h (the controller's protocol), GeniSpi.c/.h (the SPI engine), Se2Bus.h (clocks, SPI by hand), TouchMatrix.h (frames
to peaks and positions), TouchTrack.h (contacts with stable IDs, hysteresis, palm rejection), TouchGesture.h (contacts to pointer
reports), TouchHid.h (the descriptor and reports), TouchPace.h (the frame pace: a frame read only when announced; the CPU latency
hold of a burst),
TouchPower.h (suspend and the display off). Build: tooling/k27/build-native-piano-touch.sh (host tests, the pinned panel images
staged into out/, the unit's private touchscreen calibration turned into an ignored header when PIANO_TOUCH_CALIBRATION names it,
the objects, the resolved descriptor).

Frames and battery: a frame is read only when the controller announces it with a gpio162 edge, never by a timer (2026-11-12: reads
without one sent the idling controller into its watchdog state, download after download); at rest the latch is looked at once per
piano-touch-idle ms (default 50; TouchPace.h) or, with piano-touch-irq=1, its edge taken from the TLMM summary interrupt
(package/common/PianoIrq.h); an idling controller announces about one frame every 1.5 s. docs/drivers/touch.md section 13,
docs/power-irq.md.

Level 1 ran on the tablet on 2026-11-11 (bus, ID, download, frames; nobody touched the panel); level 2 is host-tested, not run.
