
Measured 2026-11-11 06:50 (owner pressing, live over the USB console): vol+ (PM8550 GPIO6, active low) down/up -> consumer
reports accepted (handleReport 0); vol- (PON bit 6, PON [10] 0x50) down/up; power (PON bit 7, 0x90) down/up (masked in that run
with piano-input-mask=6, so not reported to macOS). 0 read errors.

Battery (2026-11-11, not run yet): the poll adapts, 10 ms while a key is down or changing and for 1 s after, every
piano-input-idle ms otherwise (default 40; 0: the old 10 ms); presses of 82 ms or more are still caught (Buttons.h PollPlan,
host-tested). Stage 3 also logs the PON and GPIO6 interrupt registers once, read only. Every minute: `PianoInput: idle: N
wakeups in 60 s, irq 0 (...)`. The PMIC-arbiter interrupt path (SPI 746 through the PDC) is described in docs/power-irq.md 2.4.
