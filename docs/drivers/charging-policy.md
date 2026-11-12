# Charging policy: ChargePolicy (what PianoPower decides about the charger)

Status 2026-11-11: `package/PianoPower/ChargePolicy.{h,c}` written and host-tested (`package/PianoPower/tests/policy_host.cpp`,
also under ASan/UBSan), compiled with the kernel's C recipe (no undefined symbol beyond the signed-overflow hooks PianoPower.o
already links with), and **wired into PianoPower level 2 behind `piano-power-policy=1`** (default 0: PianoMca.c's built-in policy,
unchanged) through `package/PianoPower/ChargeLink.h`, host-tested against the DSP model of `tests/power_host.cpp` (§8, §10);
linked into a test KC; **not run on the tablet** (images and expected lines: §14). Backlog B4 (`docs/BACKLOG-hardware.md`).
The evening runs of 2026-11-11 ([charging.md](charging.md), "Measured 2026-11-11 evening") showed the input path is open with the DSP
running (397 mA at 5.2 V while 0x2008 reads 100): the input limit is now worth writing.
**2026-11-12:** run on the tablet (P1, P2, pol-chargesleep): the firmware takes 0x2008 and the bus follows it (900 → 897 mA, 1600 →
1594 mA; [charging.md](charging.md) "Measured 2026-11-12"). Since then (built, host-tested, not run): the Type-C current **in steps**
up to 3 A (§15), the input limit written before the charge current when it rises, a write's answer awaited 3 s and a late one still
taken (§15.3), and the daily profile's charging level 2 (§16; images `ram-chg-*`, §15.5).
**2026-11-12 afternoon (§17):** the 0 mA of `chg-ramp` and `chg-late` is a charger that does not charge, not a system that takes the
input and not a stale gauge: every native boot stops charging at about 40 s of uptime, before the DSP starts, and the firmware keeps
the battery at 0 mA once it runs. Built and host-tested, not run: the charging restart (0x2003 = 0 then 1, `piano-power-kick`, default
on with the policy), the charge current of a trusted Rp (1500 mA), the steps measured against the firmware's own limit (0x2029) and a
`charging:` balance line every report; images `ram-chg-kick` and `ram-chg-kick-idle` (§17.5).

## 0. The answer

- One pure function, `piano_charge_decide(config, input, state, plan)`: (source type, PDOs, charge, temperatures, limits, time,
  what the firmware reads back) → (the writes to make, in order; the charging-hold level; a re-sync request; events for the log).
  `piano_charge_ack()` feeds back each answer, `piano_charge_shutdown()` covers driver stop. No I/O, clock, allocation, floating
  point or libc (the kernel object has no undefined symbol of its own), so PianoPower can drive it from the 5 s MCA cycle, the
  30 s report, a notification, `setPowerState`, alike.
- With `piano-power-policy=1` it replaces PianoMca.c's `policy()` (§8). PianoMca keeps the codec, the one-request queue, the board
  configuration (always first), the port notifications and the UCSI probe.
- A computer port (CDP) gets the CDP row, 900 mA, whatever its Type-C Rp advertises; `piano-power-rp=1` trusts a 1.5 / 3.0 A Rp,
  since 2026-11-12 in steps from the row up to the Rp current (3 A at most, `piano-power-rp-max`), each step only while the bus holds
  and the input is used up, one step back for the boot on a sag, a quick detach or a refusal (§15; `piano-power-ramp=0`: the old
  immediate limit within 1600 mA, §13). The board thermistors PianoThermal publishes only ever make it more conservative (§1).
- Defaults are today's caution: ceilings ICL 1600 / FCC 1500 mA (PianoMca's allowlist), 5 V only, no float change, Preserve
  Battery Health off. Every planned write passes `piano_charge_write_allowed()`; each new write has a read-back (§7).
- A charger that keeps both packs at 0 mA while the input has current to spare is restarted (0x2003 = 0, 5 s, then 1, the charge
  current again), at most three times per attach, two minutes apart (§17; `piano-power-kick=0` leaves it out).

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
| charger (`charger1`) | 85 °C or more: input limit **500 mA** at most; only a reading taken after charger_pd came up (its pin is the DSP firmware's: before it runs the channel reads ~60 °C at an idle board, [thermal.md](thermal.md) §12.10) | it reads below 75 °C |

**The Type-C current** (`piano-power-rp=1`, §13): a source without a PD contract (SDP, CDP, DCP, float, Type-C) whose Rp advertises
1.5 A or 3.0 A gets min(Rp, ceiling) when that is above its row; only while a connector thermistor reads and every one that reads is
below 50 °C; never during a sleep (the row). A bus below 4.5 V under such a limit, or two detaches within 30 s of one taking, end the
trust for the boot (`RP_DISTRUST`). PD sources keep their contract's 5 V offer. **Stepped** (`piano-power-ramp=1`, the default with
`piano-power-rp=1` since 2026-11-12, §15): from the row in steps of 1500, 2000, 2500, 3000 mA up to min(Rp, `piano-power-rp-max`);
up to 1500 mA without a connector reading (BC 1.2's CDP/DCP current and the 1.5 A Rp, which the DCP row takes without a thermistor),
above it only with one below 50 °C; the sag, the quick detach and a refusal each cost one step for the boot instead of the trust.

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
| 0x2008 input limit | 4 | 100..ceiling (A 1600, B 2500; the stepped Type-C current up to 3000, §15) | attach, type / 9 V / re-send, steps, sleep, detach (100) | 0x2008 (the requested value) ±50 mA, two disagreeing → again (three writes at most); verdict: the gauges' current and the bus current 0x20004 |
| 0x2005 charge current | 4 | 100..ceiling (A 1500, B 3500) | the same, thermal steps | none (reads 0): the acknowledgement; verdict: positive gauges' current ≤ the value at 40–90 % |
| 0x2003 charge enable | 1 | **0** and 1 | 0: battery-health stop, sleep stop, the charging restart (§17); 1: lifting our stop | 0x2003 every cycle; `STOP_LEAK`; the restart: the packs' current |
| 0x2007 float voltage | 4 (mV) | 4400; 4480, 4060 with `full_charge` | float option | 0x2007 = 2 × mV − 3600 every cycle |
| 0x21018 fixed PDO | 4 | 5000; 9000 with `hv` | §2 | bus 8.0–9.6 V within 15 s / 3 decisions, else 5 V and blocked |
| 0x21017 PPS APDO | 4 | `450 << 16 \| mA/50`, 500..3000 mA, `hv` = 2 | §2 | the same |

**The codec's allowlist with the policy** (`piano_mca_write_allowed_for`, checked by `piano_mca_queue` before a write is queued
and again by `piano_mca_next` before it is built; the policy's own `piano_charge_write_allowed` is checked first): PianoMca's
(0x2008 100–1600, up to `piano-power-rp-max` (≤ 3000) only with the stepped Type-C current (`piano_mca_allow_icl`, set from the
policy's `piano_charge_icl_max`), 0x2005 100–1500, 0x2003 = 1, 0x21018 = 5000, the three board values), plus 0x2003 = 0 **only with battery health
or the charging restart on** (§17), 0x21018 = 9000 only with `piano-power-hv` ≥ 1, 0x21017 only as the 9 V word (500–3000 mA) with `piano-power-hv=2`. Not
admitted: 0x2007 (the float option is not offered: `piano-power-fv` does not exist yet), 0x2019 (the built-in policy's opt-in
un-suspend; it read 0 on 2026-11-11), stage B ceilings (not offered). A write the codec refuses goes back to the policy as refused
(three: given up until the next attach). Never written: 0x2012 AICL, 0x21002 verify (nothing here authenticates), 0x2100c data
role, the MiPPS VDMs 0x21003–0x2100a. Order inside a plan: stop,
charge current, input limit, bus voltage, resume, float; since 2026-11-12 an input limit that **rises** goes before the charge
current (the system gets the input first; the first charge current write made while the input was still at the firmware's 100 mA
request was answered late twice, every one after the input limit at once: §15.3). A plan is idempotent: a write the owner could not queue comes back at the
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
7. **Boot arguments**: `piano-power-policy=1`; `piano-power-rp=1` (§13) with `piano-power-ramp=0|1` (1, the default: the steps of
   §15) and `piano-power-rp-max=MA` (1500..3000, default 3000); `piano-power-write-ms=MS` (1000..5000, default 3000: how long a write's
   answer is awaited, §15.3); `piano-power-health=1`, `-health-stop=N`, `-health-resume=N`
   (§5); `piano-power-hv=0|1|2` (2 adds the APDO); `piano-power-kick=0|1` (1, the default: the charging restart, §17);
   `piano-power-write=0` decides for the log only (nothing is sent). Not offered yet:
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
2026-11-12 (§15): `policy_host` `testRamp` (the step functions, the allowlist's ceiling, the climb 900 → 3000 at one step per 15 s
against a 0.21 Ω source, a sag, a quick detach, a refusal, the input not used up, no bus current, a low bus, no connector reading,
writes off, ramp off, a board hold mid-climb) and the random walk with the steps, a source resistance and a sagging source;
`power_host` `testMcaLate` (the codec's late answers, unmatched answers, the ICL bound) and `testLinkRamp` (the same against the
DSP model whose bus follows the limit with the Mac port's 5.23 V and 0.21 Ω, a source that gives way above 2.2 A, a port that trips
above 2.1 A, a firmware that refuses above 2000 mA, `piano-power-rp-max=2000`, a late answer, a DCP and a PD source), and the link's
random walk with steps, loads, resistances, trips and late answers.

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

## 15. The stepped Type-C current and the write answers (2026-11-12)

Why ([charging.md](charging.md) "Measured 2026-11-12" has the evidence and what is inferred): on the Mac's port the PMIC alone
(level 1) gives about 1.5 A in (inferred), P2's 1.6 A (measured: 1594 mA) left the packs at −46 mA at the login window, and the
port advertises 3.0 A on its Rp. More input is the only lever on the cable, and the firmware's own input-current limiting is off
(0x2012 = 0): nothing walks a limit down that the source or the cable cannot hold. So the policy finds the limit itself, upwards
only on evidence, and remembers for the boot where it failed.

### 15.1 The steps (`piano-power-rp=1`, `piano-power-ramp=1`, ChargePolicy.c `ramp()`)

| Rule | Value | Why |
|---|---|---|
| Target | min(the Rp current, `piano-power-rp-max` (3000), the board holds); a source without a PD contract only | the source's own advertisement (USB Type-C: the Type-C current before BC 1.2); 3.0 A is the most a 5 V Rp can say |
| Without a connector reading below 50 °C | at most 1500 mA (`RP_WITHHELD` once) | BC 1.2's CDP/DCP current and the 1.5 A Rp: what the DCP row takes without any thermistor |
| First step | the class row (CDP 900, DCP 1500, …) | what Linux and the earlier runs wrote first; I and P1 had 100 → 900 answered at once |
| Steps | 1500, 2000, 2500, 3000 mA (≤ 600 mA each) | each firmware ramp short (§15.3); a sag found at most one step late |
| Up when | the step taken (acknowledged), held 10 s and two decisions after the decision that saw it taken (15 s with the 5 s cycle), the bus ≥ 4.65 V under it, the bus current ≥ the step − max(100 mA, step / 8) | the source has just held this much, and the system and the battery take it all, so more is of use; demand-driven: a light load never climbs |
| Down at once | a lower target (the Rp gone, a connector at 50 °C, a PD contract, a type change) | |
| Back one step, ceiling for the boot | the bus < 4.5 V under a step above the row; a detach within 30 s of a step up (planned or taken); the firmware refusing a step three times | a sag means the source or the cable is at its end; a port's over-current cut looks like a detach; a refusal is the firmware's own ceiling |
| Restart from the row | every attach, a new charger generation, a wake, a board hold (a hot connector or charger) released, 9 V | never straight back up to a step reached before |
| Sleep handover | the row (as before: nothing watches the bus while asleep) | |

The ceiling only goes down, survives detaches, generations and wakes, and ends with the boot (a different cable tomorrow starts
fresh). Measured basis for the margins (P1, P2 on the Mac's port): 5.22 V open, 5.05 V at 897 mA, 4.89 V at 1594 mA, about 0.21 Ω
from the source to the charger, so 3 A would leave about 4.6 V: above the 4.5 V sag rule, below the 4.65 V needed to go higher.
`piano-power-ramp=0` keeps the 2026-11-11 behaviour (the Rp limit at once within 1600 mA, a sag ends the trust).

The charge current stays the class's (CDP 900 mA, derated −300 mA/°C from 39 °C): the steps raise what the system can take from the
port, not how hard the cells are charged. JEITA (0–55 °C) and the 4400 mV float stay the firmware's. **Since §17** (built, not run):
while the steps follow a trusted Rp the charge current is the DCP row's within the ceiling (1500 mA), and a step counts as used up
against the firmware's own limit (0x2029) once that has settled below the step.

### 15.2 What the log says

```
PianoPower: piano-power-rp=1 piano-power-ramp=1: a source without PD that advertises 1.5 or 3.0 A on its Rp is stepped from its class row to that current, at most 3000 mA ...
PianoPower: charger writes: an answer is awaited 3000 ms (reads 1000 ms); one that comes later is still taken and logged
PianoPower: mca: write input limit (0x2008) = 900: ok (0)            (the input first when it rises, then the charge current)
PianoPower: mca: write charge current (0x2005) = 900: ok (0)
PianoPower: policy: input steps: up 900 -> 1500 mA (towards 3000 mA, Rp 3000 mA): it held 5040 mV with 897 mA taken
PianoPower: policy: input steps: at 2500 mA (towards 3000 mA): the bus is below 4.65 V under it; bus 4610 mV, 2498 mA   (once per step and reason)
PianoPower: policy: input steps: back from 2500 mA: the bus sagged below 4.5 V under it (bus 4420 mV); 2000 mA is the ceiling for the rest of this boot
PianoPower: [126s] input: bus 4720 mV 2490 mA, limit 2500 mA asked, 0x2029 2500; steps 2500 mA (holding) of 3000, ceiling 0; writes 9 ok 0 no answer 0 late, 0 unmatched
PianoPower: mca: late answer to the write of input limit (0x2008) = 1600: ok (0), after the 1000 ms window (1 late so far): handed to the policy
PianoPower: mca: an answer that matches no request: opcode 2, input limit (0x2008), sequence 41, return code 0 (in flight: 43); 1 so far
```

### 15.3 The "no answer" to 0x2005 (and 0x2008)

Evidence (2026-11-12): in P1 and P2 the first charge current write (900 mA, the first policy write after the board configuration,
the input still at the firmware's 100 mA request) got `no answer (-60)` from the codec's 1 s window; in P2 the input limit write
(100 → 1600) did too, **and the next cycle read 0x2008 = 1600 and 0x2029 = 0x4e2 (1250), then 0x640 (1600) and the bus at
1594 mA**: the write was taken and applied, only its answer missed the window (or was lost). The writes planned again were answered
`ok (0)` at once. With the built-in policy (input limit first, image I and usbhost-u0) and in pol-chargesleep (charge current 500,
about the firmware's own) no write ever went unanswered; no read ever timed out in any run.

Inferred: the firmware answers a current write once it has applied it, and 0x2029 walking 100 → 1250 → 1600 is that application
taking more than a second for a large change. Not excluded: answers lost on the way (the console cannot place the sends: USB
capture with backlog, 255-character lines that cut the diag line's "ignored" counter).

Changes: a write's answer is awaited 3 s (`piano-power-write-ms`, reads stay 1 s); the answer to a write given up on is still taken
(`McaLateWritten`: logged, handed to the policy as the acknowledgement) and the first 8 answers that match no request are logged
with their property and sequence; the input limit goes first when it rises; the steps keep each change ≤ 600 mA. A write the
firmware never answers is planned again after 10 s as before (three times, then left until the next attach). `ram-chg-late`
(§15.5) repeats P2 with the old 1 s window: `late answer` lines mean late, `no answer` without them means lost.

### 15.4 Safety summary

Never above what the source advertises (the Rp current; a 1.5 A Rp or no connector reading: 1.5 A; the class rows otherwise, PD:
its contract); every write through two allowlists (the policy's `piano_charge_write_allowed`, the codec's
`piano_mca_write_allowed_for`, both bounded by `piano_charge_icl_max` ≤ 3000); the battery's limits are the firmware's (JEITA,
float) plus the policy's derating; on any error (a refusal, a sag, a quick detach, no reading) the limit goes down or stays, never
up; the connector thermistors' 60 °C hold (100 mA) and the charger's 85 °C hold (500 mA) win over every step. Nothing here starts,
stops or restarts the DSP.

### 15.5 Test images (unattended, the Mac's cable, 600 s, RAM boots)

`tooling/k27/build-charge-test-images.sh [ramp l1 late]` builds them from a KC of this tree (`K27_KERNEL=<kernel-k27-0063-native.macho>
PIANO_GPU=2 PIANO_VMM_PRESENT=0 tooling/k27/build-native-image.sh`), the Turnip-staged seed (the login window) and the FD A. Every
image has `piano-gpu=2 piano-gpu-delay=20 panic_on_crash_disabled=1 piano-usb=2 piano-thermal=1 piano-thermal-adc=1
piano-reset-after=600` (the daily profile's drivers without the Mac's keyboard, plus PianoThermal's ADC), Data volatile, plus:

| Image | PianoPower arguments | Answers |
|---|---|---|
| `ram-chg-ramp` | `piano-power=2 piano-power-policy=1 piano-power-rp=1` (ramp, rp-max 3000, write window 3000 by default) | the candidate daily: how far the Mac's port goes, the packs' sign at the login window |
| `ram-chg-l1` | `piano-power=1` | the A/B partner: the PMIC alone in the same build and the same queue |
| `ram-chg-late` | `piano-power=2 piano-power-policy=1 piano-power-rp=1 piano-power-ramp=0 piano-power-write-ms=1000` | P2 again (1600 mA at once, now the input first) with the old window: late answers or lost ones |

Built 2026-11-12 in an agent worktree on main 04472f0 + this change (kernel 0063 `ee596f39be1ded3c…`; KC `2befbe8dc7e550e9…`,
`native kext rule: 23 native kexts linked, none from outside this repository`, `com.unixnu.driver.PianoPower: linked, ..., 800
pointers`; the Turnip-staged seed `f6176467`, so Data volatile), all four `verify-ram-images.py` PASS: `ram-chg-ramp`
`2db44271fc63d682…`, `ram-chg-l1` `dcb9f670afaace35…`, `ram-chg-late` `0ef0733577572aae…`, `ram-chg-dump` `973cf1e8d3054bb1…`
(each build has its own hash: it carries the build time as `droidvm-utc`; rebuild in the main checkout before a run).

Run in this order with `tooling/native/native-loop.sh out/ram-chg-<name>/boot-piano-macos-ram-A-bundled.img
out/ram-chg-dump/boot-piano-macos-ram-A-bundled.img` and a capture of at least 640 s. Read:
`grep -E 'PianoPower: (policy: input steps|\[[0-9]+s\] (battery|input|policy)|mca: (write|late|an answer))'`.

Pass for `ram-chg-ramp`: `charger_pd is up`, the board configuration taken, `write input limit (0x2008) = 900: ok` before the charge
current, then `input steps: up` lines while `input: bus` reads ≥ 4.65 V, the bus current following each step (within ~10 %), and from
about 150 s on `battery: ... mA (charging)` (or ≥ 0) at the login window, better than `ram-chg-l1`'s at the same time; no `FATAL`, no
`WARNING`. A `back from` line with its reason is a pass of the guard, and its ceiling is the Mac port's (or this cable's) value.
Fail: the packs still negative at 3000 mA (then the load, not the input, is the problem: P11's spinning daemons), or the bus current
not following the steps (the firmware clamps 0x2008: the `0x2029` field shows its own value).

## 16. The daily profile's charging level (decision, 2026-11-12)

**Level 2 with the stepped Type-C current** (`build-esp.py --profile daily` now defaults to `--charging 2`: `piano-power=2
piano-power-policy=1 piano-power-rp=1 piano-thermal=1 piano-thermal-adc=1`), installed only after `ram-chg-ramp` passed; until then
`--charging 1` (persistent-boot.md §11 pins v3 to it). Not "level 1 with an input-limit write". **Amended (§17):** `ram-chg-ramp`
did not pass (0 mA at 93 %), and level 1 does not charge either after the first 40 s of a boot (42 of 42 boots); the gate is now
`ram-chg-kick` (the same arguments, the restart on by default), and level 2 stays the only level that can restart the charger.

Why:
1. **Only level 2 can give more than the PMIC alone.** Level 1's input is about 1.5 A (inferred from the power balance; the PMIC's
   own configuration, unreadable without the DSP or a register map) and its charge current about 480 mA (measured: the flat
   +472..480 mA in 12 boots). Level 2 measured 1.6 A with the packs at −46 mA (level 1 at the same point: −63..−90 mA) and can step
   to 3 A, about 13 W usable on a ~7.5 W login window (with the spinning daemons) or ~10 W of ordinary use.
2. **Level 2 is proven on this tablet**: the DSP bring-up and charger_pd up in 6 of 6 boots since the charger_pd gate (11-11 20:22 to
   11-12 06:19), 0 FATAL; the board configuration, the policy's writes and their read-backs every time; the 0x2008 write followed
   by the bus within 6 mA.
3. **The level 1 write is not available safely**: the USBIN and charger blocks are this processor's (owner EE 0 in every boot), but
   there is no licence-clean PMIH010x register map, a guessed power-path write can suspend the input or brown the tablet out, and it
   would give up the firmware's JEITA, PD stack and the telemetry (bus current, 0x2029) that makes the steps safe.

The costs, accepted: (a) each boot spends 30–60 s at the firmware's ~400 mA before the first write (−1.2..−1.7 A for under a minute,
about 25 mAh); (b) a DSP crash would put the firmware back on its defaults (the 400 mA drain) until the next boot, since nothing
restarts the DSP natively: none seen in any native run (the `adsp:` report line counts fatal reports); (c) off the cable a little polling
(the 5 s charger cycle of 27 GLINK round trips, PianoAdsp's 20 ms doorbell poll: tens of wakeups a second against the ~12000 idle
entries a second measured, power.md 1.1); (d) the steps above 1.6 A are host-tested only, hence the gate.

## 17. The 0 mA of 2026-11-12 and the charging restart

Runs (the USB console logs, local, not published; `grep -a PianoPower`): `chg-ramp` (09:05, level 2, the steps of §15, 93 %),
`chg-l1` (09:27, level 1, 90 %), `chg-late` (14:59, level 2, 1600 mA at once, 81 %), `p8-idle`, `p8-idleold`, `p9-base`, `p9-off`
(15:28-16:44, level 1, 82-83 %; `p9-off` without PerfPowerServices and audiomxd, [power.md](../power.md) §1.2), and the survey of
every native boot of 11-11 and 11-12 with PianoPower (46 at level 1, 17 at level 2). **M** = measured, **I** = inferred.

### 17.1 Not a stale gauge, not a system that takes the whole input

| | `chg-ramp`, 132-582 s | `chg-late`, 132-582 s |
|---|---|---|
| Asked / the firmware's own (0x2029) | 2000 / 1750-1800 mA | 1600 / 1600 mA |
| Bus (0x20002 / 0x20004) | 4.87-4.98 V, 1334-1709 mA, mean 1466 mA: **7.2 W** (6.6-8.4) | 4.88-4.95 V, 1134-1594 mA, mean 1450 mA: **7.1 W** (5.5-7.8) |
| Both packs (gauges) | **0 mA** in all 16 reports, 7999 mAh unchanged for 8 min | 0 mA in 15 of 16 (−27 mA once, with the bus at 1591 mA) |
| The firmware's pack current (0x2027) | 0 (it read 1118 mA, discharging, before the first write) | 0 |
| The system (I: 90 % of the input less the battery) | about 6.5 W | about 6.4 W |

- **The gauges are live (M).** In the same reads the packs' voltage moves (4360-4363 mV) and their temperature climbs (36.0 →
  37.1 °C); the same gauges report a few milliamps elsewhere (−11, −13, −16 mA summed, "idle" in the log, which is our label for
  |I| ≤ 20 mA, `Gauge.h`), so their deadband is a few mA per pack; the firmware's own battery current, an independent measurement,
  reads 0 as well; the remaining capacity does not move. The units are right (+478 mA early in the same boots, −1215 mA under the
  100 mA input). BatteryStatus 0x40 is bq27z561's DSG bit: "discharging or relax mode", i.e. not in charge mode; it is 0 while the
  packs take +478 mA and turns 0x40 about 55 s after the current reaches 0 (the gauge's relax entry; 01:48 run at 5 s resolution):
  a consequence of the 0 mA, not a cause. No FC (full) or TCA (terminate-charge alarm) bit is set.
- **The input is not used up (M).** The bus current moves by up to 375 mA from report to report while both packs stay at exactly 0:
  the battery is not the slack, the input is. In `chg-ramp` the firmware's own limit sat 40-470 mA above the bus current all the way
  (mean about 310 mA): about 1.4 W at 90 % (≈ +310 mA into the packs at 4.36 V) went unused at 2000 mA asked, before any higher step.
- **The steps stopped because of it (M).** Each step goes up only when the input is used up (§15.1); with the battery taking nothing,
  the system's 1.3-1.7 A never came within 250 mA of 2000, so `steps 2000 mA (not used up)` for the rest of the run. More input would
  not have helped: the charger did not use what it had.

### 17.2 The charger stops charging at about 40 s of every native boot, before the DSP starts

- **Level 1 (M):** the PMIC alone charges at +472..+481 mA at the first reports of 36 of the 46 level-1 boots of 11-11 and 11-12
  (81-97 %; the other ten started at 0: seven at 97 %, on the float, and three at 82-93 % that followed another boot within a few
  minutes). **Of the 42 boots that logged past 45 s of PianoPower's uptime, none had the packs
  above +50 mA after it.** At 5 s resolution (01:48, 97 %): 477, 463 (28 s), 328 (33 s), 0 mA (38 s), with no discharge between, then
  0 mA for 60 s and −200 mA once the login window loads the system. In the same 5 s the DCDC block's status bytes change
  (`c3/83 19 06 0b 00 be 0a 24` → `87 00 06 0b 05 be 00 34`) and stay changed for the rest of the boot (no register map: the bytes
  are only a marker). It is not the GPU's start (20.5 s in most boots, 33.9 s in that one: the stop is at ~40 s of XNU uptime in
  both) and not the load: with `piano-cpufreq=3` the packs go negative at 20 s, but at the idle login window of `p9-off` (every CPU
  98.7-99.9 % idle, the system at its lightest) the packs read exactly 0 mA from 70 s to 890 s at 82 % and 4.24 V.
- **Level 2 (M):** the DSP's bring-up waits `piano-power-delay` (30 s) and takes another 30 s, so charger_pd comes up at 60-75 s,
  after the stop. Of the 16 level-2 boots that logged past 45 s, **none** had the packs above +50 mA after it, whatever the input:
  100 mA (−1.2 to −1.7 A), 900 mA (−0.7 to −0.9 A), 1600 mA (−173 → −46 mA, then 0), 2000 mA (0 mA).
- **What the firmware reads (M):** charging enable 0x2003 = 1, input suspend 0x2019 = 0, the charge current written (900: `ok`;
  0x2005 reads nothing back), the float 0x2007 = 5200 (4400 mV; the packs 40-170 mV below it), the battery 30-35 °C (0x202a; inside
  the firmware's 0-55 °C JEITA window and below our 38 °C derating). 0x2001 reads 0x7 in every state (discharging at 100 mA, idle at
  2000 mA, before and after every write) and so says nothing we can decode. Nothing in what is read says "not charging".
- **Inferred:** a not-charging state of the PMIC's charger latched at about 40 s of uptime, which the DSP firmware, once started,
  neither reports nor clears: it takes the limits and keeps the input on (the system runs from USB), and the battery stays at 0. What
  sets it is not known: candidates, none provable without a PMIH010x register map, are a charger watchdog left armed by the boot
  stages that nothing pets natively (our FD's charger DXE pets none, [charging.md](charging.md) §0; Linux starts the DSP within
  seconds of its boot), or a termination or inhibit decision; a fixed time after the boot stages fits the ~40 s better than any event
  of ours. Not excluded: that this firmware does not charge from a computer port (CDP) at all; Ubuntu has no computer-port result
  ([device.md](../ubuntu-history/device.md) §2.5), every native run was on the Mac's port.
- **2026-11-12 evening:** the charger watchdog is the hypothesis with the most support (the CHGR state byte reads full-on → disabled
  at the stop, the 40 s fits a 32 s bark + 8 s bite); the fix (pet it, clear "a bite disables charging") is §18.

### 17.3 The fix as built (ChargePolicy.c, `piano-power-policy=1`)

1. **The charging restart** (`kick()`, `cfg.kick`, boot argument `piano-power-kick`, default 1 with the policy). When both packs read
   within 30 mA of 0 on two gauge readings 20 s apart or more, while the bus is at 4.65 V or more and its current is 150 mA or more
   below the limit it is held to (the one taken, or 0x2029 when lower), the policy's input limit and charge current are taken and
   nothing of them is in flight, charging is allowed (battery health, its hot and cold stops), the firmware's own enable reads 1 (a
   0 of its own is never overridden), the higher pack is at most 95 % and 30 mV or more below the float voltage in force, and the
   packs read 15-45 °C: 0x2003 = 0, held 5 s (one charger cycle), then 1 by the enable's own rule (a stop of ours is lifted at the next
   decision), then the charge current again (it has no read-back), and the first gauge reading 15 s on judges it: +50 mA or more is
   `CHARGING again`, else `no change`. At most three per attach, wake or charger generation, two minutes apart; then
   `none more until the next attach`. 0x2003 is the charging enable the stock AP driver writes (one byte, as xiaomi_mca records), the
   one Ubuntu's battery protection stops and resumes with.
2. **The charge current of a trusted Rp.** While the steps follow a 1.5 / 3.0 A Rp above the class row, the charge current is the DCP
   row's (2000 mA) within the ceiling: **1500 mA** at stage A (CDP 900 before), derated −300 mA/°C from 39 °C as before. 1.5 A is
   0.17 C for the 8.6 Ah of the two packs (Ubuntu measured 1.2-1.34 A per cell at 9 V); the input limit still governs the input, the
   system first. `piano-power-ramp=0` keeps the row's.
3. **The steps against the firmware's own limit.** 0x2029 (now a policy input) is taken as the limit to use up once it has stayed
   below the step for 15 s (its highest reading at the step): in `chg-ramp` it held 1750-1800 under 2000 asked, so a charging battery
   that took the input up to it would still have left the steps at 2000. While it still rises (the firmware walks it up ~50 mA a
   cycle after a step) the step is the limit, as before. The request above it changes nothing the firmware does not allow; the sag,
   quick-detach and refusal rules are unchanged.
4. **The balance line, every report** (`piano_charge_balance()`, all levels), `chg-ramp`'s state as it would read:

   ```
   PianoPower: [282s] charging: in 1466 mA at 4911 mV (7.1 W), battery +0 mA at 4362 mV (0.0 W), system ~6.4 W; limit 2000 asked,
     0x2029 1750, enable 1; NOT CHARGING with input to spare; restarts 0          (one line on the console)
   ```

   The system is 90 % of the input less the battery (an estimate: the charger's efficiency is not read); the verdict is `charging`
   (+50 mA or more), `NOT CHARGING with input to spare` (within 50 mA of 0, 150 mA or more under the limit), `input used up by the
   system`, `draining: the system takes more than the input`, `on battery`. At level 1 it has the battery half only.

Safety (§15.4 holds): the only new write is 0x2003 = 0, admitted by the codec only with battery health or the restart on, planned
only in the decision that starts a restart; every path that cuts a restart short ends at 1 (the enable's own rule after 5 s, a detach,
the sleep handover, which now also lifts a restart's stop, the shutdown handover, a wake or a new generation, a refused or unanswered
0); the worst case is a reset (`piano-reset-after`, a panic) inside the 5 s: charging off until the next DSP start, which is the state
the restart answers anyway, with the system still on the input. The float voltage, JEITA and the input rules are untouched.

### 17.4 Tests (host)

`policy_host`: `testKick` (the 2026-11-12 state against a charger model that stays at 0 mA until its enable goes 0 and back to 1:
the 0 alone in its plan, held 5 s, the 1, the charge current again, the verdict 15 s on, the steps then on to 3000 with the battery's
share; a restart without effect: three, two minutes apart, then none until the next attach, 0x2003 at 1 each time; none near full, at
the float voltage, with the input used up, warm, cold, with the option or the writes off, with the firmware's own 0, with battery
health holding, without the gauges, on a single 0 reading; cut short by a detach, a shutdown, a wake, a generation, a refused 0, a
sleep), `testRampAicl` (the steps on past 2000 under a settled 1750; held while 0x2029 still rises), `testBalance` (`chg-ramp`'s
numbers and every verdict); the random walk with the packs at 0, a stuck charger and the firmware's own limit, checking that a 0 comes
only from battery health or a restart's start and never more than three restarts. `power_host` `testLinkKick`: the same through the
codec and the DSP model (0x2003 = 0 then 1 as one byte each through the codec's allowlist, the charge current again after it, the
steps to 2500 once the packs take current; three without effect; none with `piano-power-kick=0`; a detach and a shutdown between the 0
and the 1). Both also under ASan/UBSan.

### 17.5 Test images (unattended, the Mac's cable, 900 s, RAM boots)

`tooling/k27/build-charge-test-images.sh` (default now `kick kick-idle`), the arguments of §15.5's `ram-chg-ramp`
(`piano-power=2 piano-power-policy=1 piano-power-rp=1` and the daily drivers), 900 s:

| Image | Adds | Answers |
|---|---|---|
| `ram-chg-kick` | this tree's PianoPower (the restart on by default, 1500 mA, 0x2029, the balance line) | does 0x2003 = 0 → 1 restart charging at the login window; how far the steps go then |
| `ram-chg-kick-idle` | `k27-deny-exec=…,/usr/libexec/PerfPowerServices,/usr/libexec/audiomxd` (`PIANO_DENY_EXEC`) | the same with the idle login window's load (p9-off's), so more of the input is left for the battery |

Built 2026-11-12 in an agent worktree on main 8e64cc5 + this change (kernel 0064 `2fe50b346041ab9f…`; KC `204b760e153263e7…`,
`native kext rule: 25 native kexts linked, none from outside this repository`; the Turnip-staged seed, so Data volatile), all three
`verify-ram-images.py` PASS: `ram-chg-kick` `20c7b70cd98633cd…`, `ram-chg-kick-idle` `d4333cbfb22d2e9a…`, `ram-chg-dump`
`23b13c86912e3d51…` (each build has its own hash: rebuild in the main checkout before a run). Queue lines (`TAG IMG DUMP SECS`):
`chg-kick out/ram-chg-kick out/ram-chg-dump 930`, `chg-kick-idle out/ram-chg-kick-idle out/ram-chg-dump 930`.

Read: `grep -aE 'PianoPower: (policy: (input steps|charging restart)|\[[0-9]+s\] (battery|charging)|mca: write)'`. Pass: `charging
restart 1 of 3: both packs at 0 mA ...` after the steps settle (about 150-200 s), `charging restart 1: the packs take +N mA now:
CHARGING again`, then `charging: ... ; charging` lines to the end, `input steps: up 2000 -> 2500` (and on) once the packs take
current, the percentage rising; `kick-idle` with more battery current than `kick` at the same step. Fail: `no change` three times
(0x2003 does not restart this charger: §17.6).

### 17.6 If the restart does not restart it (attended, next)

1. Unplug and replug the cable at the login window and read the `charging:` lines: a real re-attach makes the PMIC run its detection
   and start a charge cycle; charging again says the state is the PMIC's and is cleared by an attach (then the input's own suspend,
   0x2019 = 1 then 0, is the soft replug to try, attended only: a suspend that sticks would outlive a warm reset and leave the tablet
   on battery in ABL).
2. Start the DSP before the stop (`piano-power-delay=0`, the bring-up still takes ~30 s; or a shorter firmware read): charging from
   the first write would say the stop is something the firmware prevents when it runs early, as under Linux.
3. A wall charger (DCP or PD) instead of the Mac's port: whether a computer port is the condition.

## 18. The charger watchdog (2026-11-12 evening; built, not run)

The hypothesis for §17.2's stop: the charger PMIC's BARK/BITE watchdog. In Qualcomm's SCHG charger family a host must pet a watchdog
in the MISC block; its configuration can make a bite **disable charging** (input kept on), and the boot stages leave it armed. Natively
nothing pets it (our FD's charger DXE pets none, §0 of [charging.md](charging.md); the DSP starts after the stop or not at all), so
the bite ends charging ~40 s into every boot and it stays so. **M** = measured, **I** = inferred, **F** = a register fact of another
generation of the family.

### 18.1 Register facts and where they are on piano

Read for the facts only (no code copied): Linux's `drivers/power/supply/qcom_smbx.c` (GPL-2.0, PMI8998: the family's one mainline host
driver) and the PM8150B DT (`pm8150b.dtsi`: vbus regulator @1100, Type-C @1500 for the charger @1000).

| Block, register | Fact (F) | On piano (I) |
|---|---|---|
| MISC +0x43 `BARK_BITE_WDOG_PET` | bit 0 pets | 0x2c43 |
| MISC +0x51 `WD_CFG` | 7 AFP on a bite, 6 bark irq, 5 bite irq, 4:3 safety timer after the irq, 2 the irq starts it, 1 timer on at plug-in, 0 timer on | 0x2c51 |
| MISC +0x53 `SNARL_BARK_BITE_WD_CFG` | **7 a bite disables charging**, 6:4 snarl, 3:2 bark 16 s << n (16/32/64/128), 1:0 bite 1 s << n (1/2/4/8) | 0x2c53 |
| CHGR +0x06 `BATTERY_CHARGER_STATUS_1` | 2:0 state: 0 trickle, 1 pre, 2 fast, 3 full-on, 4 taper, 5 terminated, 6 inhibited, 7 disabled | 0x2606 |
| CHGR +0x42, +0x51 | charging enable command bit 0; `CHGR_CFG2` (7 enable by command, 0 inhibit) | 0x2642, 0x2651 |
| Block order | CHGR, DCDC +0x100, BATIF +0x200, USBIN +0x300, DCIN +0x400, Type-C +0x500, MISC +0x600 | CHGR = Type-C − 0x500 = 0x2600, MISC = Type-C + 0x100 = 0x2c00 |

The Type-C block at 0x2b00 is verified (attach, CC, Rp in every boot). The MISC offsets are the same in two generations (PMI8998's
+0x643 from the charger base, the vendor SMB5 MISC + 0x43/0x51/0x53), hence the best guess for PMIH010x; this generation's own map is
not public. ChargerPmic.h's names (from SunUEFI) call 0x2600 "DCDC" and 0x2800 "USBIN"; in this layout they are CHGR and BATIF.

### 18.2 Evidence

For:
- **The state byte (M + decode I).** 0x2600 +0x06 read `c3`/`83` while the packs took +470 mA and `87` from the stop on, in this
  boot (`v3-boot2`: `dcdc c3 19 02 03 00 be 14 28` at 0 and 30 s, `87 00 0a 03 05 be 00 38` from 60 s) and in the 01:48 run at 5 s
  resolution (§17.2). Bits 2:0 go 3 → 7: **full-on → disabled**, not terminated (5) or inhibited (6), and the input stays up (the
  system runs from USB; at level 2 the firmware's bus current flows). A bite that disables charging does exactly that.
- **The timing (M).** ~40 s of uptime in every native boot, at both levels, whatever the load (idle login window or a busy boot):
  a fixed timer, not an event of ours. Bark 32 s + bite 8 s = 40 s in the family's encodings (I, a fit, not a measurement).
- **Ownership (M).** The charger complex's APIDs are EE 0's: this processor is the charger's host as far as the arbiter is concerned,
  which is where a host watchdog is expected to be petted from.
- **Level 2 does not recover (M)**: a latched bite fits a firmware that, started after it, reads its own enable 1 and changes nothing.

Not yet known (the next boot logs it): MISC's configuration itself. No log has ever read 0x2c00. The status bytes past +0x06 do not
decode sensibly with PMI8998's layout (+0x0d `28` → `38` would be "too cold"), so this generation's status map differs in places; the
MISC offsets are the guess with the most support, not a certainty. The fall over 28-38 s (477 → 463 → 328 → 0 mA) is the gauge's ~1 s
average catching a step.

### 18.3 The fix as built (`ChargerWdog.h`, PianoPower at both levels, default on)

`piano-power-wdog=2` (default): right after the SPMI probe, before the gauges (PianoPower starts in the first seconds of XNU's uptime:
its `[30s]` report still shows the packs charging; the stop is at ~40 s), PianoPower locates CHGR and MISC from the Type-C block, logs the watchdog as the boot stages left it, clears **bit 7 of
MISC +0x53 alone** (read-modify-write, read back; Linux's PMI8998 driver writes the whole register 0 and clears AFP/plug-in/bark in
`WD_CFG`; we keep the timer, its timeouts and `WD_CFG` as they are) and pets the watchdog at once, then every quarter of the bark time
(4-15 s; 8 s for a 32 s bark) from its own thread (one SPMI write; the thread already wakes every 30 s). `=1` pets only (the vendor
driver's way); `=0` reads and logs only. The clear is tried at most three times per boot, never once the DSP runs (level 2: its
firmware owns the configuration from then; the pets go on). With the bit clear a stop is also impossible while the system sleeps and
the thread cannot pet.

Gates before any write: MISC present at Type-C + 0x100, owner EE 0, TYPE 0x02 (every SCHG block's; ChargerPmic.h's charger block is
one), its configuration read; a pet only while `WD_CFG` reads armed (bit 0 or 1), the clear only then and while bit 7 is set. The
allowlist (`ChargerWdog::allowed`): MISC +0x43 = 0x01 and MISC +0x53 = (its value without bit 7); the write channel mapped is MISC's
page alone. The float voltage, charge and input currents, JEITA, the safety timer and the enable command are not written. Worst case
if the map is wrong: one write of 0x01 to a register 0x2c43 and bit 7 of 0x2c53 cleared, logged with before/after.

Log lines (all `PianoPower: `):

```
wdog: piano-power-wdog=2 (pet and clear the bite's charging stop); CHGR 0x2600 (APID 702, EE 0, type 02/..), MISC 0x2c00 (APID 708, ...)
wdog: as the boot stages left it: WD_CFG (+0x51) 0x.. (timer on, on plug-in on, ...), timeouts (+0x53) 0x.. (a bite DISABLES charging,
  bark 32 s, bite 8 s, ...); MISC rt ..; CHGR state 3 (full-on), +0x42 enable command .., +0x51 cfg2 ..
wdog: start: MISC 0x2c00 +0x53 0x87 -> 0x07 (bit 7, a bite disables charging, cleared; bark 32 s, bite 8 s kept): ok, read back 0x07
wdog: first: MISC 0x2c00 +0x43 = 0x01 (pet): ok (channel status 0x1)
wdog: petting every 8000 ms from this thread (a quarter of the 32 s bark time, 4-15 s)
charger block 0x2200: APID 698 EE 0, type 02/ba, +06 ..., rt .., +51..53 .. .. ..      (one per block 0x2200-0x2f00, once)
[30s] wdog: WD_CFG .. timeouts .. (bite does not disable charging), MISC ... ; CHGR state 3 (full-on) ...; pets 5 (0 failed), clears 1
[30s] charging: in ? (level 1: ...), battery +470 mA at 4287 mV (2.0 W); charging; CHGR state 3 (full-on); wdog pets 5
```

### 18.4 Tests (host)

`power_host` `testWdog`: piano's complex (charger block 0x2200 subtype 0xba, CHGR 0x2600, Type-C 0x2b00, MISC 0x2c00) against a model
of the watchdog armed with a 32 s bark and an 8 s bite that disables charging: without our writes the model stops at 40 s (state 3 →
7, for good); with the default the clear (0x87 → 0x07, read back) and the first pet at once, a pet every 8 s, no stop in 10 minutes,
`WD_CFG` never written, nothing but the two allowed writes; pets alone also hold it; a 16 s bark pets every 4 s; no write with MISC owned
by another EE, of another TYPE, absent, or the watchdog not armed; the bit already clear: pets only; a clear that does not read back:
three tries in all; a refusing channel: three failed pets, then none; no clear once the DSP owns the charger; no writer: every write
refused. Also under ASan/UBSan. A mutation (pet period twice the bark) fails eight checks.

### 18.5 Reading the next boot (pass / fail)

`grep -aE 'PianoPower: (wdog|charger block|\[[0-9]+s\] (battery|charging|wdog))'`.

- **Pass:** `as the boot stages left it` shows the timer on and `a bite DISABLES charging`; `start: ... -> ...: ok`; the packs stay
  at +400..+480 mA past 60 s (the `charging:` lines say `charging`, `CHGR state 3` or another charging state), percentage rising.
- **The hypothesis fails** if the watchdog reads not armed or without bit 7 (then no write is made: `wdog: not armed`), or if the
  writes are made and the stop still comes (`CHGR state 7` at ~40 s with `pets` rising). Then, in this order, from the same log
  (the block survey and the `wdog:` line already read every one): the **charging enable command** CHGR +0x42 bit 0 and its source,
  `CHGR_CFG2` +0x51 bit 7/6 (a 0 at the stop: something cleared it; the source bit set: a pin decides); **inhibit** (`CHGR_CFG2` bit
  0, state 6); **termination** (state 5, status bit 6 soft-terminate; `+0x07` changes 0x19 → 0x00 at the stop); the **safety timer**
  (`WD_CFG` 4:3 and 2: a watchdog interrupt that starts it); **AICL/input** (DCDC 0x2700 and USBIN 0x2900: +0x06..+0x0b, the ICL,
  AICL and power path status in the SMB5 layout); then §17.6 (a replug, an early DSP, a wall charger).
- **Map wrong:** MISC's TYPE not 0x02 or no APID at 0x2c00: `no write: ...`; the block survey shows where TYPE 0x02 blocks are and
  which one holds a watchdog-like +0x51..+0x53.
