# Charging policy: ChargePolicy (what PianoPower decides about the charger)

Status 2026-11-11: `package/PianoPower/ChargePolicy.{h,c}` written and host-tested (`package/PianoPower/tests/policy_host.cpp`,
also under ASan/UBSan), compiled with the kernel's C recipe (`tooling/k27/build-native-kext.py`, no undefined symbol beyond the
signed-overflow hooks PianoPower.o already links with); **not wired into PianoPower, not run on the tablet**. Backlog B4
(`docs/BACKLOG-hardware.md`). Wire it after level 2's input problem ([charging.md](charging.md) §5c–§5e) is solved: no policy
helps while the input path carries 0 mA.

## 0. The answer

- One pure function, `piano_charge_decide(config, input, state, plan)`: (source type, PDOs, charge, temperatures, limits, time,
  what the firmware reads back) → (the writes to make, in order; the charging-hold level; a re-sync request; events for the log).
  `piano_charge_ack()` feeds back each answer, `piano_charge_shutdown()` covers driver stop. No I/O, clock, allocation, floating
  point or libc (the kernel object has no undefined symbol of its own), so PianoPower can drive it from the 5 s MCA cycle, the
  30 s report, a notification, `setPowerState`, alike.
- It replaces PianoMca.c's `policy()` (the owner's change, §8). PianoMca keeps the codec, the one-request queue, the board
  configuration, the port notifications and the UCSI probe.
- Defaults are today's caution: ceilings ICL 1600 / FCC 1500 mA (PianoMca's allowlist), 5 V only, no float change, Preserve
  Battery Health off. Every planned write passes `piano_charge_write_allowed()`; each new write has a read-back (§7).

Sources: [device.md](../ubuntu-history/device.md) §2.5, [kernel.md](../ubuntu-history/kernel.md) §2.5, §3.5 (the Ubuntu history);
`yzddmr6/linux-sm8750-piano` `drivers/power/supply/xiaomi_mca.c` at `piano/dev` 7f8d6bedde47 (GPL-2.0, read for behaviour and
numbers only: nothing copied); xnu `iokit/IOKit/pwr_mgt/IOPM.h`, `RootDomain.h`, `iokit/Kernel/IOPMrootDomain.cpp` (the k27 tree).

## 1. Input limit and charge current by source (0x2000c)

| Type | Class | Input limit (0x2008) | Charge current (0x2005), 15–38 °C | Note |
|---|---|---:|---:|---|
| 1 SDP | SDP | 500 | 500 | |
| 2 CDP | CDP | 900 | 900 | Ubuntu has no computer-port result |
| 3 DCP, 5–8 QC (HVDCP 2/3/3B/3.5) | DCP | 1500 | 2000 | QC held at 5 V, as stock |
| 4 float, 15 OCP | float | 500 | 1000 | xiaomi_mca's table gives 15 1000/1000; the history's 500 is kept |
| 9 Type-C (Rp only) | Type-C | 500 | 500 | |
| 10 PD, 11 PD verified, 12 PPS, at 5 V | PD / PD PPS | min(5 V offer, 1500); 500 without PDOs | 2000 | the 5 V offer: the lowest current of the PDOs/APDOs covering 5 V |
| 10 PD at 9 V | PD | min(9 V offer, 1600) | 3000 | §2 |
| 11 / 12 at 9 V | PD PPS | min(9 V offer, 2500) | 3500 | stock in_pps / chg_pps |
| 0, 13, 14, 16+ | refused | no write | no write | the firmware's own values |

Ceilings (`icl_cap_ma`, `fcc_cap_ma`): **stage A** (default) 1600 / 1500, so DCP gets 1500 / 1500 and 9 V PD 1600 / 1500;
**stage B** 2500 / 3500 (the table as is) only after stage A is proven (§7). Absolute ceilings 2500 / 3500 hold whatever is
configured.

**Thermal derating** (charge current only; the input limit never follows the temperature, so a warm battery is not left to carry
the system). t = the warmest of the firmware's 0x202a (if −20..80 °C) and the two packs (whole °C, rounded down); none: 500.

| t (°C) | < 15 | 15–38 | 39 | 40 | 41 | 42 | … | floor |
|---|---:|---:|---:|---:|---:|---:|---|---:|
| charge current | 500 | full | full − 300 | full − 600 | full − 900 | full − 1200 | −300/°C | 500 |

Down at once; up one 300 mA step at most every 30 s, and only to the value a degree warmer would give. The firmware's own JEITA
window (0–55 °C) stays in force.

## 2. 9 V: fixed PDO and PPS APDO (opt-in, `hv` 1 or 2)

| State | Leaves when | To |
|---|---|---|
| off | PD class; a fixed 9 V PDO ≥ 100 mA (hv ≥ 1), else an APDO covering 9 V ≥ 500 mA (hv = 2, asked at min(APDO, 3000) mA as `(9000/20)<<16 \| mA/50` on 0x21017); ≥ 15 s after the attach, a wake or a new generation; past any retry time; bus < 6 V; 0x21002 reads 0 (never during an authentication, #20); charging allowed | asked (0x21018 = 9000 or the PPS word) |
| off | the bus already at 8.0–9.6 V with a 9 V offer and no request of ours (a wake without a handover) | on |
| asked | bus 8.0–9.6 V | on: the 9 V row of §1 |
| asked | not there after 15 s **and** 3 decisions, or the offer gone | blocked: 0x21018 = 5000 |
| on | bus < 6 V by itself (a Xiaomi charger does after its authentication), at most 3 times per attach | off, asked again after 10 s |
| on | anything else (bus 6–8 V, above 9.6 V, offer gone) | blocked: 5000 |
| blocked | detach; or a PD type back after an Unknown sample, within the same 3-retry budget | off |

A detach while asked (a hard reset in answer looks like one) holds the next request off for 60 s. A failed PDO read changes
nothing. 5 V is always requested as the fixed 5 V PDO, also of a PPS source (stock). The battery-health stop, the sleep and the
shutdown go back to 5 V.

## 3. PPS with the original adapter: the direct-charging curve (specification)

Ubuntu v1.1.1 charges fast through the SC8541 2:1 switched-capacitor stage from an **authenticated** Xiaomi adapter's PPS APDO;
the battery current is set by the cells' voltage (the stock step tables), the state of charge (FFC only below 95 %) and the
temperature. `piano_charge_cp_ibat_ma()` is that curve as a pure function; **nothing calls it**: a session also needs MiPPS
authentication (VDM digests the host computes; no licence-clean description of it is in this repository, open), an SC8541 driver
(online, bus over-current set 1 A above the target, a kick within 3 s or it turns itself off) and a 500 ms loop that moves the
APDO voltage in 20 mV steps to hold the bus at half the battery target. PianoPower's 5 s cycle cannot be that loop.

| Profile | When | Battery current (both cells) by the higher pack's voltage | Ends at |
|---|---|---|---|
| normal | otherwise | 8000 mA to 4150 mV, then 7230 | 4473 mV with the float option on; 4387 (4400 − 13) without |
| FFC | float option on, adapter verified, coolest ≥ 18 °C, warmest < 48 °C, both packs < 95 % | 12400 (the stage's 12 A) to 4144, 10650 to 4300, 9040 to 4475, 7230 to 4540 | 4540 mV (float 4540) |

The step only moves forward within a profile (a voltage dip under load does not raise the current again). Start: lower pack
≥ 3450 mV, higher < end − 13 mV, |pack current| < 7 A, coolest ≥ 15 °C, warmest < 38 °C. Leave: coolest < 15, warmest ≥ 45, the
end voltage, or 7 A. Warm: from 40 °C 4500 mA, held 30 s, then +500 mA per 15 s once at or below 38 °C; coolest below 18 °C:
4500 at most. Source: at most twice min(APDO current, 6000 mA), within 67 W at the voltage asked. During a session the buck sits
at 500 / 500 mA (stock moves it to 1500 / 2000 when the bus carries > 2.7 A and > 5 A is asked); after a session, 9 V on the buck
until detach. Without authentication a PPS source gets 9 V on the buck (§2): 2500 / 3500 at stage B, derated by §1.

## 4. Float voltage (opt-in, `full_charge`)

4480 mV (stock; the firmware's 4400 "leaves the fuel gauges short of Full", Ubuntu v0.10.0); 4060 mV from 48 °C until below
46 °C; no temperature counts as hot. 0x2007 is read every cycle and reads 2 × mV − 3600 (4400 → 5200, 4480 → 5360, 4060 → 4520):
a value already in force is not sent; one that reads otherwise twice in a row is written again (three writes in all at most),
then left alone (event). The option turned off after use writes 4400 once. 4540 belongs to a direct-charging FFC session only.

## 5. Preserve Battery Health (opt-in)

- **Rule.** The higher pack's charge (one charger feeds both): ≥ stop (80) → stop; ≤ resume (75) → charge; in between nothing
  changes. The mode adds Ubuntu's own window inside JEITA: warmest ≥ 55 °C stops until < 53; coolest < 0 °C stops until ≥ 2.
  No charge reading: the last decision stands for 5 min, then charging goes on (event; Ubuntu stopped on any read failure, but a
  tablet that cannot charge because a gauge read fails is worse than one charged past 80 %).
- **Stop.** 0x2003 = 0 first, then the source back to 5 V and the type's 5 V limits (the system keeps running from the input).
  **Resume.** 0x2003 = 1, only where the policy wrote the 0: a 0 the firmware set itself is reported, never overridden.
  **Read-back.** 0x2003 every cycle (two disagreeing → written again, three writes in all at most); and the verdict that matters: if the
  packs still take > 100 mA a minute after a confirmed stop, a `STOP_LEAK` event (then the firmware's bit does not mean what
  Ubuntu found; turn the mode off and report).
- **A stop never outlives the driver.** Lifted on detach (the next attach decides again within a cycle) and by
  `piano_charge_shutdown()` (stop/restart): our FD's stubbed charger DXE does not re-enable charging and a warm reset does not
  reset the PMIC (charging.md §0, §6).
- **The charging hold.** `plan.hold_awake` = writes on, mode on, charge known, a known source attached, charging allowed (below
  the limit, not hot/cold). PianoPower creates one assertion at start, `getPMRootDomain()->createPMAssertion(
  kIOPMDriverAssertionPreventSystemIdleSleepBit, kIOPMDriverAssertionLevelOff, this, "com.unixnu.driver.PianoPower.charging-hold")`,
  sets it with `setPMAssertionLevel(id, hold ? kIOPMDriverAssertionLevelOn : kIOPMDriverAssertionLevelOff)` after every decision
  and releases it in `stop()`. IOPM.h: the bit means "the system should not idle sleep. This does not prevent demand sleep";
  raising it cancels an idle sleep in progress (`evaluateAssertions` → `cancelIdlePowerDown`). The display still sleeps.
- **With s2idle** (k27 0062, `piano-sleep=1`, [power.md](../power.md) §3): while held, idle sleep never starts, so the cycle sees
  the limit. Demand sleep (Apple menu, power key, lid through PianoKeyboard's clamshell, `pmset sleepnow`, `piano-sleep-test`)
  cannot be refused by a driver: `kIOMessageCanSystemSleep`, the cancellable question, is asked of clients only for idle sleep
  (`askChangeDown`: "This must be idle sleep"). So the handover (§6) stops a protected charge for the sleep (`sleep_charge` 0, the
  default: Ubuntu v1.1.0's first model, "host SOC polling cannot run asleep"), or with `sleep_charge` 1 lets it charge on unwatched
  (to the firmware's full). Ubuntu's final model (refuse suspend, `-EBUSY`) has no macOS form; a later one would be a timed wake
  from s2idle for drivers (the kernel has `piano-sleep-max` for tests only).
- **Setting it.** Boot arguments `piano-power-limit=1`, `-limit-stop=80`, `-limit-resume=75`, `-sleep-charge=0`; at run time
  PianoPower's `setProperties()` with `ChargeLimit` (bool), `ChargeLimitStop`, `ChargeLimitResume` (numbers), from an
  administrator only (`IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator)`), validated by
  `piano_charge_config_limit()` (stop 50–100, resume 40..stop − 1, 0 = stop − 5) and published back as registry properties. No
  macOS settings pane drives it: those talk to a Mac's battery manager, which this tablet does not have (not checked on 27).
  The menu bar shows "not charging" on power (BatteryMap: IsCharging false, ExternalConnected true).

## 6. Sleep, wake, a new generation, #18, detach

| Event | Plan |
|---|---|
| sleep entry (`sleep = WILL_SLEEP`, repeated until `sleep_ready`) | with the mode on and below the limit: 0x2003 = 0; FCC = the type's 5 V current, derated (if written before); ICL = the type's 5 V limit (PD: its 5 V offer) (if written before); 0x21018 = 5000 if raised. Ready when all are taken or refused 3 times. Nothing attached, or an unknown source: ready at once (a tablet that charged once must still sleep on battery, Ubuntu e6da2db2e7bb) |
| wake (`wakes` + 1) | everything the firmware took is forgotten; `plan.resync` (PianoMca relinked: board configuration, port enable, UCSI read, as Ubuntu's rebind); attach again; limits, enable, float sent again; 9 V asked again after 15 s (Ubuntu v1.1.0: back at 9 V within 2 min after 25 min asleep) |
| new generation (charger_pd up again, a DSP restart, our relink) | the same without `resync` (PianoPower already relinks); the battery-health hysteresis and a stop of ours are kept |
| #18: the firmware's bus < 4 V while the PMIC Type-C block sees a source's Rp, ≥ 30 s and 3 decisions | `resync`, twice per episode, then a `WEDGED` event (log: replug or reboot). The episode ends when the firmware sees the source or the PMIC none |
| detach (bus < 4 V) | ICL = 100 (if written, or it reads above 150), FCC = 500 (if written), 0x2003 = 1 (if ours); the next source starts at 5 V |
| no bus reading | nothing (neither a detach nor an attach) |

PianoPower's contract (power.md §4): `setPowerState(off)` loops decide/send/ack for at most 2 s, then returns; the gauges' I2C idle.
`mach_absolute_time` stands still while suspended, so every timer here counts awake time.

## 7. The writes, the allowlist and the read-back plan (for the owner)

| Property | Bytes | Values | When | Read-back |
|---|---|---|---|---|
| 0x2008 input limit | 4 | 100..ceiling (A 1600, B 2500) | attach, type / 9 V / re-send, sleep, detach (100) | 0x2008 (the requested value) ±50 mA, two disagreeing → again (three writes at most); verdict: the gauges' current |
| 0x2005 charge current | 4 | 100..ceiling (A 1500, B 3500) | the same, thermal steps | none (reads 0): the acknowledgement; verdict: positive gauges' current ≤ the value at 40–90 % |
| 0x2003 charge enable | 1 | **0** and 1 | 0: battery-health stop, sleep stop; 1: lifting our stop | 0x2003 every cycle; `STOP_LEAK` |
| 0x2007 float voltage | 4 (mV) | 4400; 4480, 4060 with `full_charge` | float option | 0x2007 = 2 × mV − 3600 every cycle |
| 0x21018 fixed PDO | 4 | 5000; 9000 with `hv` | §2 | bus 8.0–9.6 V within 15 s / 3 decisions, else 5 V and blocked |
| 0x21017 PPS APDO | 4 | `450 << 16 \| mA/50`, 500..3000 mA, `hv` = 2 | §2 | the same |

**What the owner's allowlist must add** (`piano_mca_write_allowed`): 0x2003 value 0 (only while the mode is on — gate it on
the configuration), 0x2007 (the three values), 0x21017 (the 9 V word), and for stage B the ceilings 2500 / 3500. Unchanged and
untouched by the policy: 0x2019 = 0 (opt-in un-suspend), 0x201c / 0x201a / 0x201b (board configuration). Never written: 0x2012
AICL, 0x21002 verify (nothing here authenticates), 0x2100c data role, the MiPPS VDMs 0x21003–0x2100a. Order inside a plan: stop,
charge current, input limit, bus voltage, resume, float. A plan is idempotent: a write the owner could not queue comes back at the
next decision; one planned and unanswered for 10 s is planned again; 3 refusals of one value give up until the next attach, wake
or generation.

**Staging** (each a separate attended session, at 40–90 %, judged by the gauges' summed current, never by 0x2008):
A — the policy wired at stage A on a wall DCP and a PD charger; B — `hv` = 1 on a 9 V PD charger, then stage B ceilings;
F — `full_charge`, 0x2007 reading 5360, Full reached; L — the mode, crossing 80 % on the cable: 0x2003 reads 0 and the current
≈ 0 within two cycles, resume at 75; S — a sleep and wake on the cable with the mode on. `hv` = 2 and direct charging come last.

## 8. Wiring (the PianoPower owner's steps; nothing of this is done here)

1. `tooling/k27/build-native-piano-power.sh`: compile `ChargePolicy.c` for the host and run `tests/policy_host.cpp` (the command is
   in its header); add `"$P/ChargePolicy.c"` to the kext sources.
2. PianoMca: a switch that leaves `policy()` out, a queue entry point (`piano_mca_queue(m, property, value, bytes)` through
   the allowlist), a queue of 6, the allowlist of §7; `McaWritten` already carries `last_property` / `last_value` / `last_result`
   for `piano_charge_ack()`.
3. PianoPower.cpp, after every `McaCycleDone` (5 s, and the cycles a notification starts): fill `PianoChargeInput` (`bus_mv`
   = `value[McaBusVoltage]` / 1000, `type`, `icl_ma`, `enable`, `fv_raw` = `value[McaTermVolt]`, `verify`, `tbat_c`, `pdo`/`pdos`,
   each valid bit from `piano_mca_has()`; `generation` = `fMca.links`; the gauges from `fPack[0..1]` only when both read; `typec_source`
   from the last `PortState`), decide, queue `plan.write[]` in order, relink on `plan.resync`, set the assertion, log `plan.events`.
   The slowest cadence that works is the 30 s report (the stop overshoots by one cycle of charge, under 1 %; 9 V then takes 90 s).
4. `setPowerState(off)`: `sleep = WILL_SLEEP` until `sleep_ready` or 2 s; `setPowerState(on)` after a sleep: `wakes++`.
   `stop()` and the shutdown notification: `piano_charge_shutdown()` until ready, then release the assertion.
5. Boot arguments: `piano-power-hv=0|1|2` (exists; 2 adds the APDO), `piano-power-fv=1`, `piano-power-limit=1`,
   `piano-power-limit-stop=N`, `piano-power-limit-resume=N`, `piano-power-sleep-charge=1`, `piano-power-icl-max=N`,
   `piano-power-fcc-max=N` (stage B: 2500 / 3500); `piano-power-write=0` still plans nothing.

## 9. Deliberate differences

From PianoMca.c's policy today: charge current by type with −300 mA/°C (was 1000 mA, 1500 at 9 V, 500 from 38 °C); type 15 on the
float row (was DCP); PD without PDOs 500 (was 900); unknown types no write (was 500); 0x2003 = 1 only to lift our own 0 (was:
whenever it reads 0, 3 times per attach); timers in ms plus decision counts (were cycles). From Ubuntu: no charge reading → charging
on after 5 min (Ubuntu stops); a stop lifted on detach; demand sleep stops a protected charge instead of refusing the sleep; no
direct charging from third-party PPS sources (Ubuntu allows 4.1 A); the 5 V limits kept through sleep (Ubuntu v1.1.0, as here).

## 10. Tests (host, done)

```sh
cc -std=c11 -Wall -Wextra -Werror -c package/PianoPower/ChargePolicy.c -o /tmp/ChargePolicy.o
c++ -std=c++20 -Wall -Wextra -Werror -I package/PianoPower package/PianoPower/tests/policy_host.cpp /tmp/ChargePolicy.o -o /tmp/policy_host
/tmp/policy_host        # policy_host: all charging-policy tests passed
python3 tooling/k27/build-native-kext.py ChargePolicyCheck package/PianoPower/ChargePolicy.c -I package/PianoPower --out /tmp/cp-kcheck
```

Covered: every table row and piece; the decisions against a firmware model (writes taken or refused, read back as the real
firmware does) and a PD source model: 5 V types, refusals, lost limits, generations, thermal steps, the float voltage and its
read-back limit, fixed 9 V and PPS at 9 V with settle / block / retry / hard-reset / authentication rules, battery-health stop /
resume / leak / no charge / hot / cold, the sleep handover and the wake, #18, shutdown, the direct-charging curve; and a random walk
of 60 000 decisions checking every plan against the allowlist, the ceilings and the hold rules. The ids are checked against
PianoMca.h at compile time.

## 11. Risks

- **Unproven writes**: 0x2003 = 0 (Ubuntu's, on the same firmware family; our build may differ), 0x2007, 0x21017, charge currents
  above 1000 mA. Each is opt-in or stage-gated and has the read-back of §7; nothing is written for an unknown source.
- **A stop that outlives PianoPower** (a panic between the stop and a detach): charging stays off until the next DSP start
  (every level 2 boot starts it on its defaults). Lifted on detach and shutdown to keep the window short.
- **Demand sleep below the limit** stops charging for the sleep by default (a night on the charger at 50 % stays at 50 %).
- **The charge reading**: the higher pack decides; a gauge reading high stops early, one reading low overshoots. No reading for
  5 min ends the limit.
- **Ids**: the OS3 firmware's numbering of the PD properties (charging.md §6); type 15 named OCP by the driver's defines and DCP_B
  by its enum (one row either way).
- **Thermal**: the mode's 55 °C stop only exists with the mode on; otherwise the firmware's JEITA and the charge-current derating.
- **The curve of §3 is a specification**: untested on hardware, no authentication, no charge-pump driver.

## 12. Licence note

ChargePolicy.c/.h and policy_host.cpp are new code (BSD-2-Clause-Patent, `package/PianoPower/LICENSE`). xiaomi_mca and pmic_glink
(GPL-2.0) were read as specifications of behaviour and numbers; no code, comment or structure was copied. No vendor binary was
read.
