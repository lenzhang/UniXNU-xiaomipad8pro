
Measured 2026-11-11 06:50 (owner pressing, live over the USB console): vol+ (PM8550 GPIO6, active low) down/up -> consumer
reports accepted (handleReport 0); vol- (PON bit 6, PON [10] 0x50) down/up; power (PON bit 7, 0x90) down/up (masked in that run
with piano-input-mask=6, so not reported to macOS). 0 read errors.

Battery (2026-11-11, not run yet): the poll adapts, 10 ms while a key is down or changing and for 1 s after, every
piano-input-idle ms otherwise (default 40; 0: the old 10 ms); presses of 82 ms or more are still caught (Buttons.h PollPlan,
host-tested). Stage 3 also logs the PON and GPIO6 interrupt registers once, read only. Every minute: `PianoInput: idle: N
wakeups in 60 s, irq 0 (...)`. The PMIC-arbiter interrupt path (SPI 746 through the PDC) is described in docs/power-irq.md 2.4.
2026-11-12 (opt-in, host-tested, not run): `piano-input-dark=MS` (at most 200) stretches the idle period while the display is off
(NativeGopDisplay's `piano-display-power`, followed through its messages); a press of MS + 42 ms is always caught then, a shorter
tap can fall between two samples (buttons_host `testDarkPolling`), hence 0 by default until short presses in the dark are tested.

Power key and system sleep (2026-11-11, host-tested, not run; docs/policy.md 5.1, docs/power.md 4). With a PianoPolicy service
there (`piano-policy=N`), the power key is held back (`PowerKey.h`): a press still down after 1.5 s is a long press, the HID Power
usage down then and up at the release (PianoPolicy is not asked); released before, PianoPolicy is asked on the release
(`callPlatformFunction("piano-policy-power-key", heldMs)`): taken, nothing reaches macOS; not taken (level 1, no system sleep,
`piano-policy-powerkey=0`), the Power usage down and up at once. Without PianoPolicy, or with the power key masked
(`piano-input-mask`), the key is reported as it happens, exactly as before. Lines: `power key: PianoPolicy found: ...` (at the
first press after it appeared), `power key: short press N ms: PianoPolicy took it` / `...: macOS's (HID Power usage)`, `power key:
held 1500 ms: a long press, ...`. PianoInput is in the power tree (`pianopower::join`): `setPowerState(off)` parks the poll thread
between two samples, so no SPMI read is in flight while the kernel (k27 0062) polls the PON through the same observer channel
(`sleep N: the poll thread is parked ...; power key presses ...`); on wakes it (`wake N: polling again; the keys are read afresh
...`): the debounce starts over, a key down then is reported only after it has been up once, a press in progress is forgotten
(the release goes to macOS if it had the press). Host tests: `tests/buttons_host.cpp` `testPowerKey` (no PianoPolicy, taken, not
taken, long press, the 1.5 s threshold, other keys meanwhile, masked, PianoPolicy appearing mid-press, a sleep during a long press
and a key held through the wake).
