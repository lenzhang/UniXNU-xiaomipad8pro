PianoPolicy: piano's laptop-class policy for native macOS 27 (backlog B2 and B5), so that the drivers stay mechanisms. It owns no
hardware. It reads what PianoKeyboard (cover hall, attach, gpio95), PianoPower (the power source), NativeGopDisplay (display power),
the kernel (k27 0062's system sleep and wake mask) and a userland agent (macOS Low Power Mode) publish, decides with Policy.h, and
tells IOPMrootDomain (clamshell, clamshell sleep, AC adaptor, sleep now, user active), PianoThermal (its profile) and PianoA830 (the
GPU ceiling) through their public calls. Inert without the boot argument piano-policy (1: decisions logged, 2: act; the cover only
with piano-policy-lid=1). The page is docs/policy.md; provenance in UPSTREAM. Build: tooling/k27/build-native-piano-policy.sh (host
tests, then the objects); it is in configs/k27-native-kexts.txt.

| File | What |
|---|---|
| `PianoPolicy.cpp` | the IOService on IOPlatformExpertDevice: inputs, the thread, actions, platform functions, `machdep.piano_policy*` |
| `Policy.h` | portable: the rules, the order of the root domain's notifications, the GPU ceiling: client 1 of PianoA830's slots, or the slot shared with PianoThermal when PianoA830 has none |
| `tests/policy_host.cpp` | host tests (every cover x power x display x Low Power Mode x sleep combination, the sequences, the profile through PianoThermal's `Governor.h` to its CPU caps) |

The userland half (Low Power Mode, the lock on a cover closed on power): `tooling/native/piano-policy-agent/`.

Status (2026-11-11): built, host-tested, linked into the KC (kernels 0059 and 0062); not run on the tablet. 2026-11-12: no timer in
the normal case: NativeGopDisplay's display power is listened to (its `piano-display-power-messages`), the 5 s tick is only the
fallback for a NativeGopDisplay without messages (logged once) and `piano-policy-display-tick=1`.
