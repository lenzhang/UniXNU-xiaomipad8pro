PianoTouch: the Xiaomi Pad 8 Pro touch screen (Novatek NT36532E, host-processed THP, on QUP1 SE2 = spi@a88000, IRQ gpio162) for
native macOS 27. Inert without the boot argument piano-touch (1: bus, chip ID, firmware download, frame statistics, contacts and
gestures in the log, nothing for macOS; 2: the same and the HID pointer). macOS has no touch screen interface, so level 2 is what
it understands from a HID pointer: tap = click, hold = right click, drag = left drag, two-finger scroll, two-finger tap = right
click, palms rejected, a pinch recognised (sent as Command or Control + wheel only on request). The design, steps and boot
arguments are at the top of PianoTouch.cpp and in docs/drivers/touch.md (§14 the gestures, §15 the attended test list, §16 the
power states); provenance in UPSTREAM.

Files: Nt36532.h (the controller's protocol), GeniSpi.c/.h (the SPI engine), Se2Bus.h (clocks, SPI by hand), TouchMatrix.h (frames
to peaks and positions), TouchTrack.h (contacts with stable IDs, hysteresis, palm rejection), TouchGesture.h (contacts to pointer
reports), TouchHid.h (the descriptor and reports), TouchPace.h (the frame pace, the no-edge reads, the CPU latency hold of a burst),
TouchPower.h (suspend and the display off). Build: tooling/k27/build-native-piano-touch.sh (host tests, the pinned panel images
staged into out/, the unit's private touchscreen calibration turned into an ignored header when PIANO_TOUCH_CALIBRATION names it,
the objects, the resolved descriptor).

Battery: at rest one frame per piano-touch-idle ms (default 50; TouchPace.h), the edge by 1 ms polls or, with piano-touch-irq=1,
by gpio162 on the TLMM summary interrupt (package/common/PianoIrq.h); without an edge a frame is read anyway (blind at rest);
docs/drivers/touch.md section 13, docs/power-irq.md.

Level 1 ran on the tablet on 2026-11-11 (bus, ID, download, frames; nobody touched the panel); level 2 is host-tested, not run.
