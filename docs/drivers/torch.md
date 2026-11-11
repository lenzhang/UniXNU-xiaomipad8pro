# Rear torch: PianoTorch (B12)

Status, 2026-11-11: **built and host-tested** (`package/PianoTorch`, in the kext list, inert without `piano-torch`); **not run on the
tablet**. Level 1 is a read-only survey, level 2 is torch control. Backlog item B12 (`../BACKLOG-hardware.md`). Template: `README.md`.

## 1. Purpose and gate

A flashlight, as Ubuntu v1.1.1 has ("rear torch control, with a check tool that verifies readback and restores the previous
setting"; Ubuntu's own status: partial, torch only, camera flash left to applications). No login milestone depends on it; it is a
small daily-use item and the first native driver that writes a PMIC register on purpose, so it also proves the owner-channel write
path the later PMIC work (B4 charging policies, B6 thermistors, B7's repeater) needs.

## 2. Hardware facts

| Item | Value | Source | State |
|---|---|---|---|
| Block | PM8550 (SPMI SID 1) `led-controller@ee00`, `qcom,pm8550-flash-led`: PPID 0x1ee | boot DT `pmic@1` | verified |
| In the boot DT | **`status = "disabled"`** (as mainline's `pm8550.dtsi`): the boot DT leaves the block to the OS | boot DT | verified |
| What Ubuntu used | **the same block**: v1.1.1 (K:82a40d864b46) enables `&pm8550_flash` with one LED, `led-sources = <1 2>` ("the factory uses them as one lamp: the flash and the torch both drive the two channels together"; factory qcom,id 0/1 = channels 1/2), `led-max-microamp = 100000` (torch, the lamp), `flash-max-microamp = 400000`, `flash-max-timeout-us = 300000`; v1.2.0 (K:946e2d7c87c2) links it to the rear sensor | Ubuntu dts | verified (source) |
| Identity | TYPE +0x04 = 0x18; SUBTYPE +0x05: 0x07 four channels (this layout), 0x04/0x03 three; REVISION +0x00 | leds-qcom-flash.c | verified (source); piano's subtype: needs-boot (inferred 0x07) |
| Four-channel registers | STATUS1 +0x06 (2 bits a channel, the first = short), STATUS2 +0x07 (b0 VPH low, b1 BCL, b2/b3 BOB current limit, b4 OTST2, b5 OTST1, b6 BOB thermal overload), STATUS3 +0x09 (timer expired, 2 bits a channel); TIMER +0x3e+ch (b7 on, 10 ms steps); ITARGET +0x42+ch (b6:0); MODULE_EN +0x46 b7; IRESOLUTION +0x49 (bit ch: 5 mA steps, else 12.5 mA); STROBE +0x4a+ch (b2 hardware, b1 edge, b0 active high); CHAN_EN +0x4e (bit ch); OTST thresholds +0x7a/+0x78 (b2:0); TORCH_CLAMP +0xed (b6:0) | leds-qcom-flash.c | verified (source) |
| Torch current | per channel (ITARGET + 1) · 5 mA; Linux: ITARGET = (lamp µA / channels + 1) / 5000 − 1, clamp = (limit / channels) / 5000 − 1 = **9** for piano; safety timer off in torch mode | leds-qcom-flash.c | verified (source) |
| Write path | SPMI arbiter v7, EE 0 (`qcom,ee = 0`): owner channel at chnls `0xc500000` + 0x1000 · APID (command +0x00, status +0x08, data +0x10, opcode 0 = extended write); the arbiter accepts a write only from the APID's owner (cnfg `0xc42d000` + 4 · APID, b2:0) | boot DT; spmi-pmic-arb.c; PianoDwc3's SDAM write | verified (source; the SDAM write runs natively) |
| Owner of the flash APID | **EE 0, inferred**: Ubuntu's driver writes it, and Linux refuses writes to an APID whose owner is not its EE | spmi-pmic-arb.c | needs-boot (level 1 logs it) |
| Thermistor | `pm8550_flash_therm` = PMK8550 ADC5-gen3 channel 0x145 (a flash-thermal zone without trips) | Ubuntu dts | verified (source); reading it is a conversion request = a PMIC write: B6, not here |
| Reset values, STATUS bits latched or live, the second STATUS1 bit | — | — | needs-boot |

## 3. Ubuntu as the specification

`leds-qcom-flash.c` (`qcom_flash_led_brightness_set`, `set_flash_current`, `set_flash_timeout`, `set_flash_strobe`,
`set_flash_module_en`, `qcom_flash_fault_get`, probe's clamp): a torch on is strobe off (CHAN_EN clear) → module off →
current (ITARGET per channel, IRESOLUTION 5 mA bits) → timer 0 → module on → software strobe word 0x01 per channel and CHAN_EN set;
off is the same with 0 mA. Every write is a regmap field update (read-modify-write of that field only). The derating dance around
the flash (thresholds lowered to read OTST) is flash-only: a torch never touches the thresholds. Ubuntu's check tool
(`tools/check-piano-flash.py`, MIT) writes the LED class brightness, reads it back, restores it; `piano-camera-light-off` exists
because closing V4L2 does not turn the torch off: an explicit off path matters.

## 4. Design

**Ownership.** PianoTorch is the only writer of PPID 0x1ee; nothing else in the tree touches it. The write channel mapped is the
flash APID's one page; the arbiter itself refuses any other APID's write from that page.

**Level 1** (`piano-torch=1`): the arbiter (v7 check), the APID and owner, then 72 bytes in 8-byte observer reads (identity, status,
interrupt block 0x10–0x1f, 0x38–0x4f, 0x78–0x7f, 0xe8–0xef); only the identity read when TYPE is not 0x18. Decoded and logged raw.

**Level 2** (`piano-torch=2`) starts only when level 1 says: in the map, owner EE 0, TYPE 0x18, SUBTYPE 0x07, the survey complete,
channels 3/4 off (else "someone else drives this module"). Found lit → turned off first. Then:
- **Allowlist** (`TorchCore.h kWritable`): TIMER 0x3e/0x3f, ITARGET 0x42/0x43 (b6:0), MODULE_EN 0x46 (b7), IRESOLUTION 0x49 (b1:0),
  STROBE 0x4a/0x4b (b6:0), CHAN_EN 0x4e (b1:0), TORCH_CLAMP 0xed (b6:0). `Controller::put` refuses anything else. Every write reads
  first (on a failed read: the last known value), merges under the mask, writes, reads back and compares; a mismatch aborts the on
  and runs the off (which tries every step).
- **Current**: `piano-torch-ma` 50 (2 × 25 mA, ITARGET 4) unless a request names one; `piano-torch-max-ma` 100 = the hard cap
  (Ubuntu's DT limit, ITARGET 9, clamp 9). 5 mA a channel steps, rounded down.
- **Guard** (polled every second while lit, `TorchCore.h judge`): the request's time (default `piano-torch-timeout` 300 s, at most
  1800 s); STATUS2 over-temperature/thermal overload, current limit, low supply, STATUS1 short → off and 60 s cool-down; the
  enables no longer on → off; the hottest TSENS controller (PianoThermal's `TemperaturesDeciC`, refreshed at its report interval)
  ≥ `piano-torch-soc-max` (85 °C, its "warm") or the battery (AppleSmartBattery `Temperature`, PianoPower) ≥ `piano-torch-batt-max`
  (45 °C) → off / refused; two unreadable polls in a row → off. Missing drivers only remove their input; the time limit and the
  module's bits always apply. The time limit is software: the module's own safety timer is off for a torch (its range ends at
  1.27 s, a flash's).
- **Power**: joins the power tree (`PianoPowerContract.h`): `setPowerState(kOff)` (system sleep) and `systemWillShutdown` turn it
  off synchronously (four SPMI writes). A panic cannot: see §10.

## 5. IOKit family / ABI: the control path

`PianoTorch : IOService` on IOPlatformExpertDevice, registered (found by class). Properties: `piano-torch`, `piano-torch-survey`,
`piano-torch-state` (e.g. `on 50 mA (2 x 25 mA, itarget 4), off in 287 s`, `off (the time limit)`, `unavailable: ...`),
`piano-torch-lit`, `piano-torch-ma`.

**From macOS (chosen: the least invasive, nothing else changes).** `setProperties` accepts `piano-torch-set` (mA, 0 = off, or a
boolean) and `piano-torch-seconds` from an administrator only (`IOUserClient::clientHasPrivilege`, as PianoBacklight). The tool
`tooling/native/piano-torch/piano-torch.c` (IOKit.framework, built on the Mac like `pianousb-shell`; installed as
`/usr/local/bin/piano-torch` by `configs/userland.toml`, `docs/userland.md`): `sudo piano-torch on [MA [S]]`,
`off`, `toggle`, `status`; exit 0 when the state after the request is the one asked for. Usable from the root shell over USB too.

**From other drivers**: `callPlatformFunction` `PianoTorchSet` (mA, seconds, caller), `PianoTorchToggle` (caller),
`PianoTorchStatus` (buffer, size); queued, never blocking on the PMIC (`PianoTorchClient.h`).

**The USB console hook (specified, not made: PianoDwc3 is not changed).** In `PianoDwc3.cpp execute()`, before the "unknown
command" branch, about 15 lines: `torch [on [MA [S]] | off | status]` → `IOService::serviceMatching("PianoTorch")` +
`copyMatchingService`; `on`/`off` → `callPlatformFunction("PianoTorchSet", false, (void *)(uintptr_t)ma, (void *)(uintptr_t)s,
(void *)"the USB console", nullptr)`; then `IOSleep(50)` and `callPlatformFunction("PianoTorchStatus", false, buf, (void *)sizeof buf,
...)` printed with `cprintf`; a help line. No link dependency (PianoBacklight's pattern). A Shortcut / menu-bar item on top of the
tool is user-space work for later.

## 6. Boot args

| Argument | Default | Meaning |
|---|---|---|
| `piano-torch` | 0 | 1 survey (read only); 2 survey + control |
| `piano-torch-ma` | 50 | lamp current of an on without its own (10..max, 5 mA a channel steps) |
| `piano-torch-max-ma` | 100 | the limit (10..100; 100 is the hard cap) |
| `piano-torch-timeout` | 300 | auto-off of a request without its own time (10..1800 s) |
| `piano-torch-test` | 0 | level 2: light once at boot for S s (1..60), then off |
| `piano-torch-soc-max`, `piano-torch-batt-max` | 85, 45 | the guard's temperatures (°C) |
| `piano-torch-delay` | 0 | wait first (s) |

## 7. Kernel collection

`tooling/k27/build-native-piano-torch.sh`: host tests (`tests/torch_host.cpp`: an arbiter v7 model with the flash module and two other
peripherals; level 1 never writes and reads only PPID 0x1ee; the refusals; the on sequence's registers and values, kept bits, the
cap; a read-back mismatch, a denied write, a fault after on; every guard reason), a store check of the portable headers, the tool on a
Mac, the objects, the import check. `PianoTorch` is in `configs/k27-native-kexts.txt`; `build-native-image.sh` links it.

## 8. Hardware test plan (RAM boots, owner present for T2/T3)

```sh
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=300 PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-torch=1" \
  tooling/k27/build-native-image.sh && rm -rf out/ram-torch1 && cp -R out/ram out/ram-torch1
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=300 PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-thermal=1 piano-torch=2 piano-torch-ma=20 piano-torch-test=5 piano-torch-delay=30" \
  tooling/k27/build-native-image.sh && rm -rf out/ram-torch2 && cp -R out/ram out/ram-torch2
python3 tooling/native/usb-console.py --capture 1000 &   # then: tooling/native/native-loop.sh out/ram-torch1/boot-piano-macos-ram-A-bundled.img
grep -a "PianoTorch" logs/usb-console/<newest>-port0.txt
```

**T1 expected** (`piano-torch=1`, nothing changes; `..` = values to record):
```
PianoTorch: start: piano-torch=1 ma=50 max=100 timeout=300 s test=0 s soc-max=85 C batt-max=45 C delay=0 (PM8550 SID 1 flash 0xee00, PPID 0x1ee)
PianoTorch: level 1: loads only, nothing written (the SPMI observer's read command is the only store)
PianoTorch: stage 1: SPMI: the arbiter and the flash module's APID (read only)
PianoTorch: spmi: arbiter 0x70020000, 896 APIDs; flash PPID 0x1ee: APID .., owner EE 0 (this processor: writes possible)
PianoTorch: stage 2: the flash module's registers over the observer channel (read only)
PianoTorch: flash: revision 0x.. type 0x18 subtype 0x07: four-channel
PianoTorch: raw 00: .. (16 bytes)      raw 10: ..      raw 38: .. (24)      raw 78: ..      raw e8: ..
PianoTorch: flash: module off (00), channels on 00, 5 mA steps 0., torch clamp .. (.. mA a channel), otst thresholds ./.
PianoTorch: flash ch0 (piano's lamp): itarget .. (.. mA in .. steps) strobe .. (...) timer .. (safety timer off|on)
PianoTorch: flash ch1 (piano's lamp): ...          ch2, ch3 (not wired on piano): ...
PianoTorch: flash status1 00 status2 00 status3 00: no fault on piano's channels
PianoTorch: summary: 4ch rev .. off; APID .. owner EE 0; ch0/ch1 itarget ../.. ires .. clamp ..; status 00 00 00; writable
PianoTorch: stage 3: done
PianoTorch: done: 10 observer reads (0 failed), 0 PMIC writes
```
**T2 expected** (`piano-torch=2 piano-torch-ma=20 piano-torch-test=5`, 30 s after the kext starts; the owner watches the rear LED: a
dim light for 5 s, then dark): the T1 lines up to `summary:` (without the `level 1` line), then
```
PianoTorch: stage 3: level 2: torch control (writes to the flash module only, each read back)
PianoTorch: level 2: torch control on channels 1+2 at 20 mA (at most 100), auto-off after 300 s, off at SoC >= 85 C or battery >= 45 C; writes only to PPID 0x1ee, 10 allowlisted registers
PianoTorch: test: on at 20 mA for 5 s (piano-torch-test)
PianoTorch: found PianoThermal (its temperature is part of the guard)
PianoTorch: on: 20 mA (2 x 10 mA, itarget 1, clamp 9) for the boot test, 20 mA 5 s; 12 writes, 12 read back; status 00 00 00, enables 80 03
PianoTorch: temperatures: SoC ..C, battery 0.00 C (none)
PianoTorch: off (guard): the boot test is over; 4 writes, 4 read back
```
**T3** (attended, runtime): `piano-torch=2 piano-usb=2 piano-usb-shell=1` (+ `piano-power=1` for the battery input); the tool on the
tablet (built on the Mac by the build script: `out/native-kexts/PianoTorch/host-tests/piano-torch`, ad-hoc signed; pasted over the
root shell with `base64 -D`, or staged with `stage-soft-metal.py --file <tool>:usr/local/bin/piano-torch --boot-run "/bin/chmod 755
/usr/local/bin/piano-torch"`, as staged files are mode 644). In the root shell: `piano-torch on` → `level 2: on 50 mA (2 x 25 mA, itarget 4), off in 300 s`;
`piano-torch off` → `off (asked)`; `piano-torch on 100 10` and wait → `off (the time limit)` after 10 s; `piano-torch on 500` → 100 mA
(`on: 500 mA asked, 100 mA set`); as a plain user → `not an administrator: use sudo`; lit through 5 minutes once (default timeout)
with a finger on the lens area: warm, not hot; `lit:` lines every 30 s show status 00.

Failure lines: `owner EE n (another EE...)` / `level 2 refused: ...` (nothing written: report the survey); `off (...): a write or its
read-back failed; ... A STEP FAILED` + `failed step: register 0x.. wanted .. read ..` (a field does not hold what Linux writes:
report; the off ran); `off (guard): the module's over-temperature bits` (STATUS2 at a 20 mA torch: stop, report the raw bytes);
`lit` but no light (wiring/current: report ITARGET/IRES/clamp as read back).

## 9. Fallback

Without `piano-torch` nothing is mapped. Level 2 refusing leaves level 1's survey. Any failed on runs the full off.

## 10. Open questions and risks

1. Piano's SUBTYPE and REVISION, the owner EE, reset values → T1.
2. Are STATUS1/2/3 live or latched (a stale bit would refuse the torch)? → T1/T2 raw bytes.
3. **A panic or watchdog reset while lit**: the kext cannot run its off. A PS_HOLD warm reset resets the PMIC's peripherals
   (*inferred*, not verified for this block); ABL/Ubuntu boot next. Check once in T3 (`reset` from the console while lit).
4. The flash thermistor (ADC 0x145) as a direct guard → with B6's ADC conversions.
5. Camera flash (400 mA, 300 ms strobe) is B11's, not this driver's; it would need the derating sequence and a hardware strobe.

## 11. Licence note

Linux's `leds-qcom-flash.c`, `spmi-pmic-arb.c` and the piano dts (GPL-2.0) are read as the specification only (offsets, fields,
sequences, the current arithmetic); nothing is copied. Ubuntu's MIT tools are read for behaviour. PianoTorch is BSD-2-Clause-Patent;
provenance in `package/PianoTorch/UPSTREAM`.
