# Charging policy: ChargePolicy (what PianoPower decides about the charger)

Status 2026-11-11: `package/PianoPower/ChargePolicy.{h,c}` written and host-tested (`package/PianoPower/tests/policy_host.cpp`,
also under ASan/UBSan), compiled with the kernel's C recipe (no undefined symbol beyond the signed-overflow hooks PianoPower.o
already links with), and **wired into PianoPower level 2 behind `piano-power-policy=1`** (default 0: PianoMca.c's built-in policy,
unchanged) through `package/PianoPower/ChargeLink.h`, host-tested against the DSP model of `tests/power_host.cpp` (§8, §10);
linked into a test KC; **not run on the tablet** (images and expected lines: §14). Backlog B4 (`docs/BACKLOG-hardware.md`).
The evening runs of 2026-11-11 ([charging.md](charging.md), "Measured 2026-11-11 evening") showed the input path is open with the DSP
running (397 mA at 5.2 V while 0x2008 reads 100): the input limit is now worth writing.

## 0. The answer

- One pure function, `piano_charge_decide(config, input, state, plan)`: (source type, PDOs, charge, temperatures, limits, time,
  what the firmware reads back) → (the writes to make, in order; the charging-hold level; a re-sync request; events for the log).
  `piano_charge_ack()` feeds back each answer, `piano_charge_shutdown()` covers driver stop. No I/O, clock, allocation, floating
  point or libc (the kernel object has no undefined symbol of its own), so PianoPower can drive it from the 5 s MCA cycle, the
  30 s report, a notification, `setPowerState`, alike.
- With `piano-power-policy=1` it replaces PianoMca.c's `policy()` (§8). PianoMca keeps the codec, the one-request queue, the board
  configuration (always first), the port notifications and the UCSI probe.
- A computer port (CDP) gets the CDP row, 900 mA, whatever its Type-C Rp advertises; `piano-power-rp=1` trusts a 1.5 / 3.0 A Rp up
  to the 1600 mA ceiling (§13). The board thermistors PianoThermal publishes only ever make it more conservative (§1).
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

**The board thermistors** (PianoThermal's `piano_thermal_board_temps()`, `package/include/PianoBoardTemps.h`, through its platform
function, so PianoThermal need not be linked, enabled or present; [thermal.md](thermal.md) §12.7). Each counts only with its valid
bit; a missing one is unknown, never cool. They only ever make charging more conservative:

| Thermistor | Rule | Released |
|---|---|---|
| battery (`batt`) | joins t above as a third reading (the warmest and the coolest of all); more than 10 °C from the packs: `NTC_APART`, logged once per episode | — |
| connector (`conn`, `conn2`) | either at 60 °C or more: input limit **100 mA** (the input current heats the contact) | both read below 50 °C (one that stops reading keeps the hold) |
| charger (`charger1`) | 85 °C or more: input limit **500 mA** at most | it reads below 75 °C |

**The Type-C current** (`piano-power-rp=1`, §13): a source without a PD contract (SDP, CDP, DCP, float, Type-C) whose Rp advertises
1.5 A or 3.0 A gets min(Rp, ceiling) when that is above its row; only while a connector thermistor reads and every one that reads is
below 50 °C; never during a sleep (the row). A bus below 4.5 V under such a limit, or two detaches within 30 s of one taking, end the
trust for the boot (`RP_DISTRUST`). PD sources keep their contract's 5 V offer.

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
  the limit, not hot/cold). PianoPower creates one assertion on first use, `getPMRootDomain()->createPMAssertion(
  kIOPMDriverAssertionPreventSystemIdleSleepBit, kIOPMDriverAssertionLevelOff, this, "com.unixnu.driver.PianoPower.charging-hold")`,
  sets it with `setPMAssertionLevel(id, hold ? kIOPMDriverAssertionLevelOn : kIOPMDriverAssertionLevelOff)` after every decision
  and releases it at shutdown (the kext of the collection never stops). IOPM.h: the bit means "the system should not idle sleep. This does not prevent demand sleep";
  raising it cancels an idle sleep in progress (`evaluateAssertions` → `cancelIdlePowerDown`). The display still sleeps.
- **With s2idle** (k27 0062, `piano-sleep=1`, [power.md](../power.md) §3): while held, idle sleep never starts, so the cycle sees
  the limit. Demand sleep (Apple menu, power key, lid through PianoKeyboard's clamshell, `pmset sleepnow`, `piano-sleep-test`)
  cannot be refused by a driver: `kIOMessageCanSystemSleep`, the cancellable question, is asked of clients only for idle sleep
  (`askChangeDown`: "This must be idle sleep"). So the handover (§6) stops a protected charge for the sleep (`sleep_charge` 0, the
  default: Ubuntu v1.1.0's first model, "host SOC polling cannot run asleep"), or with `sleep_charge` 1 lets it charge on unwatched
  (to the firmware's full). Ubuntu's final model (refuse suspend, `-EBUSY`) has no macOS form; a later one would be a timed wake
  from s2idle for drivers (the kernel has `piano-sleep-max` for tests only).
- **Setting it (as built).** Boot arguments `piano-power-health=1`, `piano-power-health-stop=N` (80), `piano-power-health-resume=N`
  (75), validated by `piano_charge_config_limit()` (stop 50–100, resume 40..stop − 1, 0 = stop − 5; refused values leave the mode
  off, logged); `sleep_charge` stays 0 (not offered). Not built yet: the run-time switch through PianoPower's `setProperties()`
  (`ChargeLimit`, `ChargeLimitStop`, `ChargeLimitResume`, administrator only, published back as registry properties). No macOS
  settings pane drives it: those talk to a Mac's battery manager, which this tablet does not have (not checked on 27). The menu
  bar shows "not charging" on power (BatteryMap: IsCharging false, ExternalConnected true).

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

**The codec's allowlist with the policy** (`piano_mca_write_allowed_for`, checked by `piano_mca_queue` before a write is queued
and again by `piano_mca_next` before it is built; the policy's own `piano_charge_write_allowed` is checked first): PianoMca's
(0x2008 100–1600, 0x2005 100–1500, 0x2003 = 1, 0x21018 = 5000, the three board values), plus 0x2003 = 0 **only with battery health
on**, 0x21018 = 9000 only with `piano-power-hv` ≥ 1, 0x21017 only as the 9 V word (500–3000 mA) with `piano-power-hv=2`. Not
admitted: 0x2007 (the float option is not offered: `piano-power-fv` does not exist yet), 0x2019 (the built-in policy's opt-in
un-suspend; it read 0 on 2026-11-11), stage B ceilings (not offered). A write the codec refuses goes back to the policy as refused
(three: given up until the next attach). Never written: 0x2012 AICL, 0x21002 verify (nothing here authenticates), 0x2100c data
role, the MiPPS VDMs 0x21003–0x2100a. Order inside a plan: stop,
charge current, input limit, bus voltage, resume, float. A plan is idempotent: a write the owner could not queue comes back at the
next decision; one planned and unanswered for 10 s is planned again; 3 refusals of one value give up until the next attach, wake
or generation.

**Staging** (each a separate attended session, at 40–90 %, judged by the gauges' summed current, never by 0x2008):
A — the policy wired at stage A on a wall DCP and a PD charger; B — `hv` = 1 on a 9 V PD charger, then stage B ceilings;
F — `full_charge`, 0x2007 reading 5360, Full reached; L — the mode, crossing 80 % on the cable: 0x2003 reads 0 and the current
≈ 0 within two cycles, resume at 75; S — a sleep and wake on the cable with the mode on. `hv` = 2 and direct charging come last.

## 8. Wiring (as built 2026-11-11, `piano-power-policy=1`)

Default `piano-power-policy=0`: none of this runs, PianoMca.c's built-in policy and its log lines are as before.

1. **Build.** `tooling/k27/build-native-piano-power.sh` compiles `ChargePolicy.c` for the host, runs `tests/policy_host.cpp`, links it
   into `tests/power_host.cpp` (the wiring tests, §10) and adds it to the kext's sources.
2. **The codec** (`PianoMca.c`): `piano_mca_use_external_policy(m, stop, pps)` leaves `policy()` out and keeps `board()`;
   `piano_mca_queue(m, property, value, bytes)` queues through `piano_mca_write_allowed_for` (§7; a queue of 6, the built-in policy
   keeps its 4); `piano_mca_board_settled()`. Each `McaWritten` and a write's timeout (−60) go to `piano_charge_ack()`.
3. **The link** (`package/PianoPower/ChargeLink.h`, portable, header only). Gates: msm/adsp/charger_pd reports up (the codec's 60 s
   fallback still links the codec, but the policy waits for "up") and the board configuration of this link is taken or given up,
   so the stock board configuration always comes first. One decision per finished codec cycle (5 s, a notification, the read-back
   after writes); while a sleep or shutdown is handed over, also one per write answer. `PianoChargeInput` from the codec's last cycle
   (bus, type, limit, enable, float, verify, Tbat, PDOs; `generation` = the codec's link count), both packs only when both read
   within three report periods, the Type-C block read at each decision (the Rp current and #18), PianoThermal's four board
   temperatures. The plan's writes queued in order; a resync relinks the codec instead (the new generation sends everything); the
   input limit, the enable bit and the bus voltage request compared with the next cycle's reading (logged).
4. **PianoPower.cpp**: the link stepped from `serviceMca` before the codec's pump; every write answer logged as it arrives
   (`mca: write input limit (0x2008) = 900: ok (0)`) and handed on; events, plan changes and read-backs as `policy: ...`; a
   `[Ns] policy:` line with every report.
5. **Sleep, wake, shutdown.** PianoPower joins the power tree (`pianopower::join`, [power.md](../power.md) §4) only with the policy.
   `setPowerState(off)` raises the sleep ask and answers 3 s; the thread decides with `sleep = WILL_SLEEP` until the plan is ready (at
   most 2 s) and calls `acknowledgeSetPowerState()`; with nothing reachable (no channel, charger_pd not up, the board not through)
   at once. `setPowerState(on)` after an off is one more wake: the next decision forgets what the firmware took and asks for the
   resync (board configuration, port enable, UCSI read, everything read). A new charger generation (charger_pd up again) relinks the
   codec as before, and the policy sends everything again without a resync. `systemWillShutdown` waits up to 2.5 s for
   `piano_charge_shutdown()` (a stop of ours lifted, the source back at 5 V), then the assertion is released.
   `piano-reset-after` (PSCI, k27 0056) bypasses all of this.
   **Checked against the contract** ([power.md](../power.md) §4, 2026-11-11): off = the handover (the class's 5 V limits, 9 V back
   to 5 V, a protected charge stopped; nothing attached or nothing reachable: at once), acknowledged within the 3 s it answers (the
   contract allows 30 s); on = one more wake, the resync (#18) with the port read at the next decision; the initial `on` of the
   join counts no wake; the charging hold is off during a handover; level 1 and level 2 without the policy stay out of the power
   tree. One gap, fixed here: after the acknowledgement the thread still ran its periodic report until the freeze (the gauges'
   bit-banged I2C, the Type-C block, the RTC), so a freeze could catch a gauge read with its pins taken as GPIOs. Now
   `ChargeLink.h` `SleepReports` pauses the reports from the acknowledgement to the wake (`policy: reports paused until the wake
   ...`, then `policy: reports again ...` and the overdue report at once; host-tested `testSleepReports`). Not done, on purpose: the
   codec's DSP traffic keeps running until the freeze (Ubuntu blocks every MCA transaction until resume; here the DSP stays as it is:
   GLINK is polled, an answer that arrives while suspended waits in its FIFO and every codec timeout counts awake time only).
6. **The charging hold**: created on first use (`createPMAssertion(kIOPMDriverAssertionPreventSystemIdleSleepBit, Off, this,
   "com.unixnu.driver.PianoPower.charging-hold")`), set to `plan.hold_awake` after each decision (off during a handover), released at
   shutdown; on only with battery health on.
7. **Boot arguments**: `piano-power-policy=1`; `piano-power-rp=1` (§13); `piano-power-health=1`, `-health-stop=N`, `-health-resume=N`
   (§5); `piano-power-hv=0|1|2` (2 adds the APDO); `piano-power-write=0` decides for the log only (nothing is sent). Not offered yet:
   `piano-power-fv`, the stage B ceilings, `sleep_charge`, the `setProperties()` switch. `piano-power-unsuspend` stays the built-in
   policy's.

## 9. Deliberate differences

From PianoMca.c's policy today: charge current by type with −300 mA/°C (was 1000 mA, 1500 at 9 V, 500 from 38 °C); type 15 on the
float row (was DCP); PD without PDOs 500 (was 900); unknown types no write (was 500); 0x2003 = 1 only to lift our own 0 (was:
whenever it reads 0, 3 times per attach); timers in ms plus decision counts (were cycles). From Ubuntu: no charge reading → charging
on after 5 min (Ubuntu stops); a stop lifted on detach; demand sleep stops a protected charge instead of refusing the sleep; no
direct charging from third-party PPS sources (Ubuntu allows 4.1 A); the 5 V limits kept through sleep (Ubuntu v1.1.0, as here).

## 10. Tests (host, done)

`tooling/k27/build-native-piano-power.sh` runs both (`policy_host: all charging-policy tests passed`, `power_host: all passed`);
alone:

```sh
cc -std=c11 -Wall -Wextra -Werror -c package/PianoPower/ChargePolicy.c -o /tmp/ChargePolicy.o
c++ -std=c++20 -Wall -Wextra -Werror -I package/PianoPower package/PianoPower/tests/policy_host.cpp /tmp/ChargePolicy.o -o /tmp/policy_host
/tmp/policy_host        # policy_host: all charging-policy tests passed
```

**The wiring** (`tests/power_host.cpp`, `testLink*`): ChargeLink and the real codec against the DSP model (it answers reads, takes or
refuses writes, reads them back as the firmware does), stepped as PianoPower steps it, defaulting to the 2026-11-11 Mac port (CDP, 5.23 V,
Rp 3.0 A, 97 %, 29 °C, 100 mA requested): the board configuration before any policy write, then FCC before ICL; CDP 900 / 900 with
the Rp ignored, read back, nothing more while it reads back, a lost limit written again after the second disagreeing cycle; write=0
sends nothing; Rp 3.0 A → 1600, 1.5 A → 1500, the default → 900, a sag below 4.5 V and two quick detaches → distrusted for the boot,
no PianoThermal or a 52 °C connector → the row, a PD source → its contract; PD with hv 1 → 1500 at 5 V, 9 V after 15 s, 1600, the
bus read back; detach → 100 / 500 once and the row again on the next attach; a wake → one relink, the board configuration and every
limit again; a new generation → the same without a relink; charger_pd not up → no policy write, the board refused → the policy only
after the five tries; a write timeout → −60 to the policy and the write planned again; battery health 85 % → the stop first and read
back, 75 % → the resume and the hold, stale packs → no hold, health off → the codec refuses a 0; the sleep handover (stop for the
sleep, ready, the wake resumes), nothing attached or no charger_pd → ready at once, shutdown lifts our stop; connector 61 °C → 100 mA
held while a channel is missing and released below 50 on both, charger 86 °C → 500 until below 75, the battery thermistor at 45 °C →
500 mA charge current and the gap noted; and a random walk (12 × 300 periods over options, sources, Rp, temperatures, wakes,
generations, sleeps, refusals) in which every write the model saw passes the codec's allowlist and the stage A ceilings.
`policy_host.cpp` adds the Rp row function, the sag, the board holds and the thermistor's derating as pure tests, and the Rp and the
board temperatures to its random walk (an Rp limit only with the option, a connector reading and no PD contract; a connector hold
always wins). Both pass under ASan/UBSan.

Covered: every table row and piece; the decisions against a firmware model (writes taken or refused, read back as the real
firmware does) and a PD source model: 5 V types, refusals, lost limits, generations, thermal steps, the float voltage and its
read-back limit, fixed 9 V and PPS at 9 V with settle / block / retry / hard-reset / authentication rules, battery-health stop /
resume / leak / no charge / hot / cold, the sleep handover and the wake, #18, shutdown, the direct-charging curve; and a random walk
of 60 000 decisions checking every plan against the allowlist, the ceilings and the hold rules. The ids are checked against
PianoMca.h at compile time.

## 11. Risks

- **The wiring is untested on the tablet.** The power-tree join, the 3 s sleep acknowledgement and `systemWillShutdown` run only with
  `piano-power-policy=1`; the ADT's `sleep-disabled` keeps every native image awake, so the handover and the wake path have only the
  host tests. `piano-reset-after` (PSCI) bypasses the shutdown handover: writes made by a test stay in the PMIC across the reset
  (0x2008 = 900 is harmless; a battery-health stop is not, §14).
- **The Rp limit** (opt-in): a cable's Rp is indistinguishable from the port's; with the firmware's AICL off, a source that cannot
  hold 1.6 A is caught only by the 4.5 V sag rule or by two detaches, at the 5 s cycle. A port's over-current cut costs one or two
  reconnects. It needs PianoThermal's ADC (`piano-thermal-adc=1`, not yet run on hardware) for a connector temperature.
- **The board thermistors** are not stock-verified thresholds (thermal.md §12.7's proposal); a connector channel that fails while
  hot holds 100 mA for the boot (by design).

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

## 13. A computer port: the CDP row by default, the Rp current opt-in

On 2026-11-11 the Mac's port read type CDP (0x2000c = 2) on the firmware's side and Rp 3.0 A on the PMIC's Type-C block (sink status 0x02,
the decode corrected 2026-11-11). The USB Type-C specification ranks a sink's sources of current: a PD contract, then the Type-C
current (1.5 / 3.0 A on Rp), then BC 1.2 (SDP 500 mA; CDP and DCP up to 1.5 A), then the default USB power; so 3.0 A is what the port
itself offers. The default is still the policy's CDP row (900 mA, Ubuntu's and xiaomi_mca's value) and the Rp current is opt-in
(`piano-power-rp=1`, at most the 1600 mA ceiling), because:

1. **Nothing walks a too-high limit down.** The firmware's input-current limiting reads off (0x2012 = 0, status 0x64): a source that
   cannot hold the limit sags or trips its over-current cut instead of being met halfway. The policy's guards act at its 5 s cycle,
   not at the specification's 60 ms for a sink to follow an Rp change.
2. **The Rp may be the cable's.** A legacy USB-A to C cable carries its own Rp; a non-compliant one (10 kΩ) advertises 3.0 A on any
   port, a 500 mA one included. The PMIC cannot tell.
3. **One reading.** The decode was reversed until 2026-11-11; one session read 0x02 on one port. Ubuntu never used the Rp current.
4. **The connector carries it**: 1.6 A is above every row but DCP's, so it is taken only while a connector thermistor reads below
   50 °C (unknown is never cool).
5. **The first question is whether the firmware honours 0x2008 at all** (the 07:28 run took 900 and still drew 0 mA): the row answers
   it at the lower current.

**The budget** (the 2026-11-11 login screen on the Mac's port, image G2, 97 %, 4.35 V): the input 0.397 A × 5.21 V ≈ 2.1 W and the packs
−1.6 to −1.7 A ≈ 7.1 W, so the system draws about 9 W. At about 90 % conversion, 900 mA in brings about 4.2 W (packs ≈ −1.1 A),
1600 mA about 7.3 W (≈ −0.4 A); the packs break even at about 2.0 A in, or with the system under about 4 W (900 mA) or 7 W (1600 mA).
The charge current is the CDP row's 900 mA, derated from 39 °C (the packs reached 37.7 °C on 2026-11-11). So "battery ≥ 0 at the login
screen" on this port needs the Rp limit and a quieter system (display dimmed, GPU idle), or a wall charger.

## 14. Test images and expected lines (owner present, the Mac's port, RAM boots)

Built in the main checkout's style (`PIANO_K27_TREE` from `~/.config/piano/env`); each command leaves the image in `out/ram`, copy it
aside before the next:

```sh
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=600 \
  PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-power=2 piano-power-policy=1" tooling/k27/build-native-image.sh
mkdir -p out/ram-power/P1 && cp out/ram/boot-piano-macos-ram-A-bundled.img out/ram/build-A-bundled.json out/ram-power/P1/
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=600 PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-power=2 \
  piano-power-policy=1 piano-power-rp=1 piano-thermal=1 piano-thermal-adc=1" tooling/k27/build-native-image.sh
mkdir -p out/ram-power/P2 && cp out/ram/boot-piano-macos-ram-A-bundled.img out/ram/build-A-bundled.json out/ram-power/P2/
tooling/native/native-loop.sh out/ram-power/P1/boot-piano-macos-ram-A-bundled.img out/ram-dump/boot-piano-macos-ram-A-bundled.img
grep -E 'PianoPower: (policy|mca: write|\[[0-9]+s\] (diag:|battery|policy))' logs/native-runs/<ts>-loop/xnu-console.txt
```

| Image | Extra arguments | sha256 | Answers |
|---|---|---|---|
| P1 | none | `38518896e3c6dca0…` | the CDP row: does the firmware take 0x2008 = 900 and draw more than 397 mA |
| P2 | `piano-power-rp=1 piano-thermal=1 piano-thermal-adc=1` | `95af1c43f9874116…` | the Rp limit (1600 mA) with the connector thermistors read |

Both with `panic_on_crash_disabled=1 piano-usb=2 piano-power=2 piano-power-policy=1` and `PIANO_GPU=2 PIANO_VMM_PRESENT=0
PIANO_RESET_AFTER=600` (so also `piano-gpu=2 piano-gpu-delay=20`, `piano-reset-after=600`, Data volatile: the A830-staged seed).
Built 2026-11-11 in an agent worktree on main a40b611 + this change (main f94932b adds nothing to the KC or to these images): KC
`710a133f1d160bcd…` (kernel 0059 `d475f824a4f234af…`; `native kext rule: 21 native kexts linked, none from outside this
repository`; `com.unixnu.driver.PianoPower: linked, ..., 782 pointers`), both verify PASS. An image's hash changes with every build
(it carries its build time): rebuild in the main checkout before a run.

**P1** (the CDP row), after the lines of §5e up to `msm/adsp/charger_pd is up`:

```
PianoPower: piano-power-policy=1: ChargePolicy decides the charger writes, after msm/adsp/charger_pd is up and the board configuration is through: ...
PianoPower: piano-power-rp=0: the Type-C Rp current is not trusted: the class rows (a CDP gets 900 mA whatever its Rp says)
PianoPower: policy: joined the power tree (0): sleep hands the charger over, a wake sends everything again
PianoPower: policy: waiting for the board configuration to be taken (or given up) before the first policy write
PianoPower: mca: write CP state (0x201c) = 1: ok (0)          (and OTG configuration, CID configuration = 1800)
PianoPower: mca: board configuration taken (CP state, OTG configuration, CID): the policy follows
PianoPower: policy: no PianoThermal service: the board temperatures are unknown (never cool: no Rp limit); looked up every 10 s
PianoPower: policy: a source is attached: bus 5208 mV, type CDP, Type-C Rp 3000 mA
PianoPower: [75s] policy: CDP, input limit 900 mA (row), charge current 900 mA, 5 V, charging, hold off; 1 decisions, 2 writes queued, ...
PianoPower: mca: write charge current (0x2005) = 900: ok (0)
PianoPower: mca: write input limit (0x2008) = 900: ok (0)
PianoPower: policy: input limit (0x2008) reads 900 after the write of 900: as written
PianoPower: [80s] diag: ..., bus current (0x20004) 8xx mA, input limit (0x2008) 900 mA; ...
PianoPower: [98s] battery: 4380 mV, -1100 mA (discharging), 97 % ...
```

Pass: the write `ok (0)` and read back as written; the firmware's **bus current above the 397 mA of 2026-11-11** (up to about 900 mA at a
9 W load); the packs' current above that run's −1.6 A. The battery at or above 0 mA at the login screen is the goal, which by §13's
budget P1 reaches only with the system under about 4 W; at that run's 9 W expect about −1.1 A. The bus current still at about
400 mA with 0x2008 reading 900 means the firmware does not take its input limit from 0x2008 on this port: stop there and report
(the next suspects are the AICL state and the CDP handling of the firmware, not more current).

**P2** (the Rp current; run after P1 passed, and after thermal.md §12.8's ADC test or with it): additionally PianoThermal's own
`adc:` lines, then

```
PianoPower: policy: PianoThermal found: its board temperatures (battery, charger, connectors) join the rules
PianoPower: policy: board temperatures running, valid 0xf: battery 31, charger 33, connector 30, connector 2 30 C (disabled 0)
PianoPower: [75s] policy: CDP, input limit 1600 mA (Rp, 3000 mA advertised), charge current 900 mA, 5 V, charging, hold off; ...; Rp trusted; ...
PianoPower: mca: write input limit (0x2008) = 1600: ok (0)
PianoPower: policy: input limit (0x2008) reads 1600 after the write of 1600: as written
```

Pass: the bus current up to about 1.5 A with the bus above 4.5 V, the packs at about −0.4 A at a 9 W login screen and positive once
the system is quieter. `the source advertises 3000 mA on its Rp, but no connector temperature reads below 50 C` means the ADC gave
no connector reading (its own lines say why): the row stays. `the Rp limit is not trusted for the rest of the boot` means the bus
sagged under 1.6 A or the port cut out: the Mac's port does not deliver its Rp current through this cable.

**Battery health** (`piano-power-health=1`) is not built as a default image: at 97 % it stops charging at once (0x2003 = 0), and the
reset at the end of a RAM test bypasses the shutdown handover, so the stop would stay in the PMIC until the next DSP start. Run it as
§7's session L, attended, at 80–90 %, and unplug the cable before the reset (a detach lifts the stop).
