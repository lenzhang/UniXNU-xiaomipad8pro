# Ubuntu on piano, device repository history: power, charging, display, input

What the Ubuntu project for the same tablet did about power management and battery life, when, why, and what it
depended on in its kernel; then what that means for the native macOS port. Read 2026-11-11.

Sources (all public):

- **Device repo** `yzddmr6/xiaomipad-8pro-mainline`, full history (594 commits, 2026-09-21 .. 2026-11-10, tags
  v0.1.0-preview .. v1.2.0). Cited as `D:<hash>` and repo-relative paths. Release notes are
  `packaging/release/changes/vX.Y.Z.md` (the same text as the GitHub release bodies). Internal notes deleted from the
  tree were read from history: `docs/CHARGING-TEST.md` (at `D:5ecddf7`, `D:471eee5`), `docs/suspend-check.zh-CN.md` and
  `docs/APPROACH.zh-CN.md` (at `be2ebc3^`), `docs/PIANO8..11.md`.
- **Issues/PRs** of the device repo: #3, #18, #20, #21 and PR #22 matter here (cited `#N`).
- **Kernel repo** `yzddmr6/linux-sm8750-piano`: commit messages and the pin files only, cited `K:<hash>` (12 chars).
  Which kernel commit each release ran is in the device repo's `kernel/abi.json` and `kernel/source.json`. A separate
  page studies the kernel code. Behaviour is described here, not copied (the kernel is GPL-2.0).

No battery-life figures (idle drain in mA, suspend drain per hour, runtime in hours) were published anywhere in the
device repo, its releases or its issues. The only power measurements are the ones quoted per topic below.

## 0. Top findings

1. **Charging 0 mA (our level 2).** Linux never recorded the DSP's charger firmware holding the input at 0 mA. With no
   host write it requests 100 mA and charge current 0 and "charges at about 500 mA whatever the source"; with the
   requested limit reading 100 mA the input was measured at 394 mA (K:1e4a0fc89fee, K:fd03bee1a1ee). Linux never writes
   input suspend (0x2019) or AICL (0x2012). What Linux does and we do not: the stock board-configuration handshake
   (CP_STATE 0x201c, OTG_CFG 0x201a, CID_CFG 0x201b) on every DSP service generation; all clients gated on the PDR "up"
   of `msm/adsp/charger_pd`; the port-notification (PAN) enable and ack sent as USB-C *write requests* (opcode 0x15,
   cmd 0x10 / 0x11) on owner 0x800c; a UCSI client; and tests at 40–90 % battery on wall chargers. The history has no
   SDP/CDP (computer port) charging result at all. §2.5, §3.1.
2. **Suspend design.** s2idle only (`SuspendState=freeze`). Since v0.8.0 the whole SoC reaches CX collapse with DDR in
   self-refresh *inside s2idle*. That needed every block to drop its RPMh/clock votes: LPASS macro clocks under runtime
   PM, the LLCC island ways reserved (without that, every suspend **reset the SoC**), PCIe D3, UFS lane clocks, GENI
   force suspend, the GMU bound to a driver, no RF clock vote. Wake sources: RTC, power key, keyboard status pin
   (TLMM 95), cover open (hall, TLMM 80). §2.1–2.2.
3. **Default policies.** 120 Hz (saves power over 144 Hz); suspend after 5 idle minutes on battery; power key = lock;
   cover closed on external power = lock, not sleep; Balanced profile = schedutil with the prime cores capped at
   4.0896 GHz (no boost) and the GPU at 160–1100 MHz; CPU hotspot throttling at 105 °C. §2.3, §2.4, §2.6, §2.7.

## 1. Timeline (power-relevant changes only)

Kernel = the `7.2.x-pianoNN` build each release shipped (`kernel/abi.json`).

| Version (date) | Kernel | Power-relevant changes | Kernel dependencies (examples) |
|---|---|---|---|
| v0.1.0-preview (10-11) | 7.2.6-piano7 | Firmware scanout only; **every sleep state refused** by policy (D:60ea283: a stuck resume could only be cleared by a forced reboot); battery read-only | fuel gauges, read-only `xiaomi_mca` (K:1e4a0fc89fee) |
| v0.2.0 (10-12) | piano11 | Native DSI 3200×2136 @ 144 Hz, backlight, DPMS; touch follows the panel power sequence; charger read-only: "the battery discharges under Linux, also on USB" (README at D:bb9bdbe) | KTZ8866 dual chip (K:f4a810d07ea2), writable ICL/FCC (K:fd03bee1a1ee, unused) |
| (internal, 10-13) | piano12–17 | An open touch stream survives device suspend; "actual s2idle rebooted the tablet and a later diagnostic exposed a Wi-Fi resume timeout" (D:7176369); PCIe sleep fixes (D:6420ab2); gauge PM fix (D:ea8e026). Sleep still disabled | K:0fc5e1634469, K:31f73d72b1b8 (PCIe wake paths), K:13a696ff6c3b (bq27xxx vs sleep) |
| v0.3.0 (10-14) | piano24 | **Suspend enabled (s2idle)**: RTC, power key, keyboard, cover-open wake; closing the cover does not wake; 6 RTC cycles. **Charging control** (policy service); current limit handed back and restored across suspend; display shutdown order; keyboard wake with the screen off | K:a988f93ff6ad, K:f768a5585abc (SC8541), K:b96cd03f088b (lid), K:f15d206ba17e and K:64f228ab0389 (keyboard), K:4e8489b1c0df (DPU stop order) |
| v0.3.1 (10-15) | piano25 | No garbled frame on screen-off/lock/suspend (shared display controller no longer reset while the panel scans; 42 DPMS cycles, 0 underruns); DSI PHY interface-clock runtime PM; power-key action configurable, default suspend | imported community DSI/audio PM fixes |
| v0.4.0 (10-15) | piano26 | Touch restarts asynchronously after panel enable/resume: screen wake ~0.3 s faster; 60/60 DPMS cycles | asynchronous THP start |
| v0.5.0 (10-17) | piano27 | USB host/OTG and **charging while using a powered dock**; power key = **lock**; GNOME Power Saver/Balanced/Performance (TuneD) | K:cf4d132fdee4, K:27490ca3d1f4 (board config), K:b1eb75bcf65f (dwc3 ↔ UCSI), K:c78194b27b55 (CPU capacity), K:d86e3f7b2204 (GMU init timeout) |
| v0.6.0 (10-22) | piano32 | USB3 in both orientations, DP Alt Mode; the touch controller is told about external power | K:55b55246a1da (QMP), K:bd04ba0e1289 (THP charger state) |
| v0.6.1 (10-24) | piano35 | CPU hotspot limit 105 °C (stock); LLCC enabled; touch-frame washout fixed by a CPU latency limit during GPI DMA | K:549976b18fb6, K:fe98a3eca32d, K:322fd1bfbda9 |
| v0.6.2 (10-25) | piano38 | Rare boot with no sound, sensors **or charger/USB-C** fixed: the first GLINK message is no longer dropped | K:235ab34e2a6e |
| v0.7.0 (10-26) | piano40 | 11 PMIC board thermistors exposed | PMIC ADC |
| v0.8.0 (10-28) | piano43 | **Deep sleep in standby**: "the main CX logic supply is switched off and memory is in self-refresh"; RTC offset kept in the PMIC; **120 Hz default, 5-minute idle suspend on battery** (D:e070564) | 25 commits (§2.1): K:1d5861e2ee37, K:52280a6d4ed5, K:333826b0a11b, K:4248c3226460, K:09b81da85469, K:7291c862688c, K:bcfcfb70be9c, K:f46f8386ed88, K:7dffd78b342d, K:e6da2db2e7bb |
| v0.9.0–v0.9.2 (10-31..11-01) | piano44–51 | Second DSI link brought up as stock does; its clock now runs continuously while the screen is on (it used to stop and restart ~174 times per frame); keyboard touchpad auto-detected | K:5ef2173e6417 |
| v0.10.0 (11-04) | piano55 | **Charges to 100 %**; termination voltage lowered at 48 °C | K:bdd0228f991f |
| v1.1.0 (11-07) | piano58 | **Preserve Battery Health** (80 % stop, 75 % resume; stays awake while charging below the limit); **cover closed on external power only locks**; plugged-in sleep keeps 5 V limits by charger type; tsens/GMU/DP/UCSI fixes | K:ea5c83ee493f, K:80eeae96d596, K:e9acb111dc96, K:08e750858038, K:2a5ccfa32100, K:97567648b85d |
| v1.1.1 (11-09) | piano64 | **PPS fast charging** with in-kernel MiPPS; no keyboard re-wake right after screen-on; a closed cover no longer wakes; ambient-light fix; DSP sleep-state notifier (opt-in, off) | K:f5b1de558983, K:6824e7cdc219, K:afe204252af2, K:1cca070836d2, K:19114681e92c |
| v1.2.0 (11-10) | piano73 | GENI UART IRQ masked across the whole sleep transition; PDC SPI trigger-type sync withdrawn; GSD ambient-light fix shipped, the automatic-brightness claim withheld from the final notes | K:a2b4feb14264, K:386c1f901aa0, K:ff6cf0aafb8f, `ubuntu/gnome-settings-daemon/ambient-zero.patch` |

## 2. Topics

### 2.1 System suspend

**What.** `packaging/piano-device-support/usr/lib/systemd/sleep.conf.d/50-piano-sleep.conf`: `SuspendState=freeze`
(s2idle); hibernation, hybrid sleep and suspend-then-hibernate off (no resumable swap). The kernel also offers PSCI
`deep` (`/sys/power/mem_sleep` reads `s2idle [deep]`, #21), but the product never selects it. GNOME/logind defaults
apply apart from the overrides in §2.3.

**When and why.**
- v0.1.0: everything refused (D:60ea283). RTC wake tests then exposed the blockers one by one: a real s2idle rebooted the
  tablet, Wi-Fi did not resume (D:7176369; the PM diagnostic build was withdrawn after isolating WLAN recovery failures),
  PCIe needed its wake paths kept (D:6420ab2), the gauges had to stop I2C around sleep (D:ea8e026), and "battery and
  charging PM lifecycle issues" remained (D:6420ab2).
- v0.3.0: enabled (D:8cbd0fd) after the charging, battery, keyboard and display resume fixes passed. The acceptance tool
  is `tools/piano-suspend-check.py` (D:1992456): exact `suspend_stats` deltas, BOOTTIME − MONOTONIC ≥ 80 % of the RTC
  interval, the wake IRQ found from `/proc/interrupts` deltas (the kernels have no `pm_wakeup_irq`, D:7dcbeee), then
  peripherals after resume (Wi-Fi gateway ping, Bluetooth powered, sound card, gauges, `xiaomi-mca-usb` readable, keyboard
  I2C trace clean, DSI-1 on). An opt-in mode also requires charging to come back (D:7ad036f).
- v0.8.0: **deep state reached.** Until then the SoC never collapsed CX in sleep. The kernel series (piano40 → piano43):
  - The LPASS VA/TX macro codecs manage their clocks with runtime PM (100 ms autosuspend), so the LPASS_HW_MACRO and
    LPASS_HW_DCODEC votes drop when audio is idle (K:52280a6d4ed5, K:333826b0a11b, FROMLIST K:e95745885cdf, K:c5508d8393b4).
  - **LLCC ISLAND1 (SCID 12) ways reserved** (K:1d5861e2ee37). With the votes released and the upstream SM8750 table
    (island ways only as bonus ways), *all 16 suspend cycles ended in a PS_HOLD reset* (15 s2idle, one PSCI system
    suspend); holding the macro vote avoided the reset, but then CX did not collapse. With the ways reserved (as in the
    downstream table) 6/6 s2idle cycles of 48–60 s resumed, "with the cxsd and ddr residencies within 0.1 s of the time asleep".
  - PCIe root ports may enter D3 (K:4248c3226460); the native PCIe hotplug driver is built (K:c553a9503af7).
  - UFS lane-clock helpers no longer take extra references on shared clocks, which had blocked CXO shutdown (K:09b81da85469).
  - GENI UARTs force-suspend in system sleep even with an open file, else their clocks and interconnect stay on (K:7291c862688c).
  - The GMU gets a driver (sync_state) and the GPU's cx power-domain link becomes stateless (K:bcfcfb70be9c, K:5e69b7737f3a).
  - The WCN7860 power sequencer no longer votes RPMH_RF_CLK1, which held the RF buffer on through sleep (K:f46f8386ed88).
  - dwc3: the VBUS override write is deferred during system suspend. Unplugging the cable in s2idle ran the UCSI role
    switch before the glue clocks were back, the QSCRATCH access never completed and **the SoC reset** (K:7dffd78b342d).
  - Also in the series: unused crypto engine and inline crypto engine disabled (K:2ee29b15aa99, K:000b34528897).
- v1.1.0: the thermal sensors' lower-threshold interrupts are masked across suspend. Suspending from a warm state
  otherwise refires a wake-armed level interrupt that aborts every attempt or wakes s2idle at once (K:08e750858038).
- v1.1.1 / v1.2.0: the GENI UART IRQ is masked across the complete transition and drained before shutdown; a level
  interrupt answered with IRQ_NONE had the Bluetooth UART disabled as "unhandled" (K:a2b4feb14264, K:386c1f901aa0). The DSP
  sleep-state SMP2P notifier was written after "an ADSP fatal error was observed after suspend" (a FastRPC listener
  assertion) and is **disabled by default** pending validation (K:19114681e92c, K:a504cd537e94, K:2ad18a4de500).

**Measured.** CX and DDR residency ≈ time asleep (K:1d5861e2ee37). No drain figure was published. The suspend checker notes
that with a USB cable to a host attached the HS PHY cannot enter L2 (`HS-PHY not in L2`): "it costs suspend power with the
cable attached".

**Pitfalls seen.** SoC resets (LLCC table, dwc3 VBUS override); Wi-Fi resume timeout; charging lost after resume (§2.5,
#18); USB hub and DP broken after resume (#21, open: the QMP PHY / DWC3 resume fails before the xHCI and SMMU errors, and
the DP link clock fails to enable); between v0.3.0 and v0.8.0 a tablet that had charged once could not suspend on battery
(`cannot quiesce charging for system sleep: -95`, fixed by K:e6da2db2e7bb); early wakes from TLMM 80/95 (§2.13).

### 2.2 Wake sources

| Source | Line | Behaviour | Since |
|---|---|---|---|
| RTC alarm | PMIC RTC | timers and the test tool | v0.3.0 |
| Power key | PMIC PON | wake; from v0.5.0 a short press locks, or wakes the lock screen | v0.3.0 |
| Keyboard | WN8030 status pin, TLMM 95, rising edge | KEY_WAKEUP only while the panel is blanked; not armed while the cover is closed (K:afe204252af2); from v1.1.1 the I2C data IRQ is **no longer** a wake IRQ (K:6824e7cdc219): it had woken the tablet right after suspending from a just-lit screen | v0.3.0 (K:f15d206ba17e) |
| Cover open | hall, TLMM 80 active low, PDC 122, `gpio-keys` SW_LID | wake on opening only (EV_ACT_DEASSERTED = rising edge); closing never wakes (K:b96cd03f088b) | v0.3.0 |
| DSP | SMP2P sleep-state inbound IRQ | only with the opt-in notifier | v1.1.1 (off) |

The v1.1.0 known limitation "pressing on the closed cover can wake the screen" was fixed by not arming keyboard wake while
SW_LID reads closed (K:afe204252af2) and by matching piano's lid device, which reports SW_LID alone (K:1cca070836d2).

### 2.3 Idle and button policy

`packaging/piano-ubuntu-desktop/usr/share/glib-2.0/schemas/50_piano-ubuntu-desktop.gschema.override`:
`power-button-action='lock'` (a patched GSD adds the enum value 4 `lock`, `ubuntu/gnome-settings-daemon/power-button-lock.patch`;
before v0.5.0 the default was suspend, D:f51ca2b) and `sleep-inactive-battery-timeout=300` (v0.8.0, D:e070564: "suspend
after five idle minutes instead of fifteen"). Idle on AC keeps GNOME's default.

`packaging/piano-device-support/usr/lib/systemd/logind.conf.d/60-piano-lid.conf` (v1.1.0, D:1b917e5):
`HandleLidSwitchExternalPower=lock`. Why: "Stock Android keeps the tablet awake while it is plugged in"; with battery
protection the kernel refuses suspend while it still has to stop charging at the limit; logind had already announced the
suspend, GNOME relit the panel, and the lock screen stayed lit under the closed cover for about a minute. On battery the
cover still suspends.

### 2.4 Display, refresh rate, backlight, display off

- **120 Hz default** since v0.8.0 (D:e070564, `packaging/piano-ubuntu-desktop/etc/xdg/monitors.xml`): "120 Hz saves power
  over 144 Hz, which stays selectable". History: the first plan was 120 Hz because upstream DPU computed a 577.3 MHz MDP
  clock for 144 Hz, above the catalog's 575 MHz (`docs/APPROACH.zh-CN.md` §3.1, deleted); native 144 Hz worked by v0.2.0
  and was the default (D:fc84f1e) until v0.8.0. Touch broke at 120/60 Hz in v0.3.0–v0.3.2 (#2, fixed in v0.4.0).
- **Display off.** v0.3.0: the video pipeline is stopped before the timing engine (K:4e8489b1c0df) and the bonded slave DSI
  clock state is restored (K:c6a971f9bedb). v0.3.1: the shared display controller is not reset while the panel still scans.
  v0.9.1: the DSI link clock runs continuously while the screen is on (stability over a little clock gating).
- **Backlight KTZ8866** (K:f4a810d07ea2, in v0.2.0): two chips, each driving half the LED strings; the primary references the
  secondary and writes every change to both; **keep the bootloader's state** (read BL_EN and the brightness at probe, write
  only if the backlight is off or the chips disagree, set the current sinks read-modify-write so BL_EN is not cleared);
  **never drive HWEN low**: it resets every register, *including the LCD bias the panel runs from*; FLAG is read-to-clear.
- Touch and keyboard follow the panel's blank/unblank (§2.9, §2.10); Mutter's `PowerSaveMode` is the DPMS switch.

### 2.5 Charging

**Architecture.** The charger is Xiaomi's "MCA" firmware in the ADSP charger PD, reached over pmic_glink on the
battery-manager owner (0x800a) with Xiaomi's property protocol instead of Qualcomm battmgr (K:cc36810ca732: battmgr got
280-byte replies with non-zero return codes and timed out on every read). UCSI (owner 0x800b) and AltMode/PAN (0x800c) are
the standard clients. Two BQ27Z561 gauges on the AP's I2C report the battery; an SC8541 (SE5, 0x6a) is the charge pump.
pmic_glink keeps the PDR lookup of `msm/adsp/charger_pd` "the clients depend on"; AltMode only starts on its "up".

**DSP defaults** (K:fd03bee1a1ee, K:1e4a0fc89fee and the module-parameter descriptions in the driver): requested input
limit 100 mA (also what stock restores on detach), charge current 0; "left alone, the ADSP charges the battery at about
500 mA whatever the source"; float voltage 4400 mV (0x2007 reads `2·mV − 3600`, i.e. 5200); its JEITA window (0–55 °C)
stays in the firmware. The 0x2008 readback is "the limit requested from the charger, which need not be the one in force:
with the input at 394 mA it read 100"; 0x20004 "reads 0 once charging is done". The firmware rejected battmgr's
notification request and sent nothing unsolicited in 125 s, so the driver polls every 2–3 s.

**Userspace switch.** `packaging/piano-device-support/usr/lib/systemd/system/piano-charging.service` (D:471eee5; `full_charge`
added by D:3476e65), started by udev on `xiaomi-mca-usb` add (`usr/lib/udev/rules.d/80-piano-charging.rules`): load
`pmic_glink_altmode`, set the `xiaomi_mca` parameters `hv_charge charge_current full_charge mipps_auth cp_charge` to 1, set
`pmic_glink_altmode.pan_ack=1`, and enable `charge_policy` last. **Stopping the unit sets `charge_policy=0`, and the driver
hands the charger back to the DSP defaults** (the mechanism behind our "never `systemctl isolate`" drain). Every parameter
defaults off, so an older boot set never sees unknown module options.

**Policy** (in the kernel; port of the blu-sharky community series, K:d998b2273637; values from the stock `strategy_buckchg`):

| Source (0x2000c) | Input limit | Charge current |
|---|---:|---:|
| SDP / CDP | 500 / 900 mA | 500 / 900 mA |
| DCP, QC at 5 V, PD at 5 V | 1500 mA (PD: at most its 5 V PDO) | 2000 mA |
| FLOAT / OCP | 500 mA (community fallback) | 1000 mA |
| Type-C (Rp only) | 500 mA | 500 mA |
| Fixed PD 9 V (bus must settle ≥ 8.0 V) | 1600 mA | 3000 mA |
| PPS or verified PD at 9 V | 2500 mA | 3500 mA |
| Unknown / reserved | writes refused | refused |

The charge current derates by 300 mA per °C above 38 °C down to 500 mA, is 500 mA below 15 °C, and steps back up at most
every 30 s. SC8541 direct charging uses PPS only (at first started below 4.10 V and left at 4.20 V or 45 °C; v1.1.1 moved
to the stock charging tables per CP session and FFC, K:e20ddf530f90, K:9662a7f87f77). `full_charge` (v0.10.0,
K:bdd0228f991f): stock 4480 mV, 4060 mV from 48 °C, back below 46 °C, because "the firmware default float voltage is 4400 mV,
which leaves the fuel gauges short of Full". MiPPS adapter authentication: userspace helper (D:471eee5) replaced by the
kernel (K:f5b1de558983, D:5fcd9df, v1.1.1). The firmware reads the adapter identity **only as DFP**: the helper checked the
SVID before requesting DFP and deadlocked (#20, PR #22); an interrupted sequence left VBUS at ~5 V with `online=1` and both
cells slightly discharging, recovered only by a replug.

**Board-configuration handshake** (v0.5.0, K:cf4d132fdee4, K:27490ca3d1f4, K:de5a8c2b874f). On every DSP service generation,
before any policy and independent of `charge_policy`, the driver writes what stock `qcom_subpmic_sync_cfg_work` writes:
CP_STATE 0x201c (1 byte: charge pump present), OTG_CFG 0x201a (16 bytes, the words 0, 1, 0xffffffff, 0xffffffff: "the
charger boost and PMIC GPIO controller, with no external GPIOs"), CID_CFG 0x201b (u32 0x708, a firmware identifier taken
from the stock DT: "the firmware initializes PMIC/GPIO CID detection and registers its IRQ"), retried until all succeed.
It arrived with USB host/OTG, after charging already worked on wall chargers (v0.3.0), so Ubuntu did not show it is needed
for sink charging; K:27490ca3d1f4 also says it "does not claim to fix the observed source-detach/reinsert detection failure".

**PAN** (USB-C port notifications). AltMode sends the enable as a USB-C write request (opcode 0x15, cmd 0x10, arg 0) on owner
0x800c once charger_pd is up; each notification (opcode 0x16) is acknowledged with opcode 0x15, cmd 0x11, arg = port index;
one write outstanding at a time, 5 s ack timeout. `pan_ack` (from blu-sharky eae63ac2f462) acks the piano ports that have no
DT connector, which nothing else would.

**Battery protection** (v1.1.0). `usr/lib/udev/rules.d/80-piano-charge-limit.rules` sets UPower's `CHARGE_LIMIT=75,80` on both
gauges; the kernel provides shared start/end thresholds (K:ea5c83ee493f) and stops by CP stop, buck disable 0x2003, back to
5 V. It first stopped charging at suspend ("host SOC polling cannot run asleep"), then switched to the stock
`online_wake_lock` model: one wakeup source held only while protection is on, wired power is present and charging is
allowed, with prepare returning -EBUSY meanwhile (K:80eeae96d596). Hence the lid-lock rule (§2.3) and the v1.1.0 limitation
that Suspend from the menu does not suspend while plugged in below the limit.

**Charging across suspend.** v0.3.0 handed back 100 mA and 5 V before sleep (K:a988f93ff6ad: "No hardware PM result is
claimed"). #18 (v0.10.0): after some s2idle resumes the firmware reported `online 0` and VBUS 0, ignored replugs (no new PD
negotiation) and the touch driver sent "charger state 0"; 2 of 3 overnight resumes lost charging; only a reboot or rebinding
`xiaomi_mca` recovered it (a rebind re-sends the board configuration, the defaults and the policy; 9 V returned ~20 s
later). v1.1.0 keeps the attached source type's 5 V limits during sleep instead of 100 mA (K:e9acb111dc96), plus CP-stop
and UCSI-registration error handling (K:118eebfdd943, K:2a5ccfa32100); the owner's test: 25 min asleep on the 67 W adapter,
back at 9 V within 2 min. v1.1.1 finishes authentication cleanup before sleep and re-authenticates after resume (K:f7388e42cb05).

**Measured.** Standard PD input ~8.86 V × 1.59 A (#20). The README marked charging "partial" again in v1.1.0 because
fast-charge power stays below stock (D:dcb9094). Tests always ran at 40–90 % battery on the wall charger
(`docs/CHARGING-TEST.md`: "at 100% the rising-percentage PASS criterion cannot be met"; "Do not interpret `Charging`,
service `active`, or an authentication result as net battery charge": net charge is the gauges' current). Powered docks
charge since v0.5.0. **No SDP/CDP (computer port) charging result appears anywhere in the history.**

### 2.6 GNOME power profiles

`tuned-ppd` with three MIT TuneD profiles (`device/power/tuned/`, D:0a7914d, v0.5.0); Balanced is the default; battery and
AC map alike (`ppd.conf`). All use `schedutil` and the same minima (policy0 384 MHz, policy6 1017.6 MHz, GPU 160 MHz).

| Profile | policy0 max (CPUs 0–5) | policy6 max (CPUs 6–7) | boost | GPU max |
|---|---:|---:|---:|---:|
| Power Saver | 1785.6 MHz | 1958.4 MHz | off | 342 MHz |
| Balanced | 3532.8 MHz | 4089.6 MHz | off | 1100 MHz |
| Performance | 3532.8 MHz | 4320.0 MHz | on | 1100 MHz |

No platform profile, no governor change, no disk or network tunables. Checked on the device; persists across a cold boot.

### 2.7 CPU idle, DVFS, thermal

- cpuidle is PSCI, under the same Gunyah as ours. K:322fd1bfbda9 names the SM8750 system low-power domain state (PSCI
  parameter 0x0200c354, **4400 µs** exit latency) and the cluster state (**1983 µs**). Entering the domain state stalls GPI
  DMA: NT36532E 5450-byte reads took 2.46 or 4.85 ms instead of ~2 ms and caused a whole-screen washout frame on the TDDI
  panel. The fix (v0.6.1) is a 2000 µs CPU latency limit held only while a GPI DMA SPI message runs; a DDR bandwidth vote
  alone did not help.
- Energy model: CPU capacities and dynamic-power coefficients in DT (K:c78194b27b55, v0.5.0); LLCC driver built in (v0.6.1).
- **Thermal** is kernel-only (no thermald or other daemon). The nine CPU hotspot passive trips moved from 95 °C to the stock
  105 °C with 5 °C hysteresis and 10 ms polling while passive (K:549976b18fb6). Why: one prime-core load took a zone from
  34.5 to 91 °C in 0.26 s (~200 °C/s) and step_wise oscillated between 3.51 and 4.20 GHz; stock runs the prime cores at
  4.32 GHz with these zones at a median 102.4 °C and never above 105.5 °C over 2410 samples. The 118 °C zones and the
  120 °C hot / 125 °C critical trips are unchanged. The 11 PMIC board thermistors (xo, msm, flash, wifi, display,
  backlight, conn, conn2, battery, quiet, charger1) are for monitoring only (v0.7.0). Charging has its own thermal rules (§2.5).

### 2.8 GPU power (A830)

GPU devfreq floor 160 MHz in every profile (§2.6). Votes and GMU: MX dependency votes built from the GPU OPP table (later
replaced by upstream's "drm/msm/a6xx: Fix RPMH dependency votes"); **GMXC never voted at retention** while the GX GDSC is on:
SM8750's gmxc table starts 0, 16 (retention), 50, so the enable corner was retention; the fix uses the first level above
(50), as the downstream MIN_SVS floor does (K:925bbbba6eca; a GX bandwidth HFI timeout once followed such votes, without
shown causation). GMU firmware init timeout raised (v0.5.0, K:d86e3f7b2204). v0.8.0: GMU bound to a driver, cx link
stateless (§2.1). v1.1.0: mask the HFI IRQ before booting the GMU, clear only the CM3 fault bit in the HFI IRQ, bound the
wait for a fault devcoredump, **drop the boot bus vote once the GMU votes bandwidth** (K:ce9c0ed98aab, K:891bc63f703a,
K:6565c5044f6d, K:97567648b85d).

### 2.9 Keyboard (WN8030), touchpad, hall

Kernel only (`device/keyboard/README.md`; no service): HID keyboard and consumer keys, LEDs (`kbd_backlight` 0–100,
micmute), `SW_TABLET_MODE` = inverse of the MCU's connected bit (GNOME locks rotation while attached).

- **Power follows the panel** (K:64f228ab0389, sequence taken from the stock module's behaviour): on blank send command
  0x25 = 0, then assert the sleep line; on unblank release the sleep line, wait for the data-ready line (TLMM 97) for up to
  100 ms (plus 5 ms only if it had to wait), drain pending frames, send 0x25 = 1, restore LEDs and backlight. No reset or
  firmware reload on timeout.
- The community's 12-second periodic keep-awake was removed: stock keeps the accessory awake 10 s only during
  authentication (K:24142165d4f9).
- Wake: §2.2. Touchpad: auto-detected with the DT property as a hint, retried after a failed creation (v0.9.0,
  K:5ef2173e6417, K:38b9f656ef06).
- Hall: TLMM 80 through `gpio-keys` SW_LID (§2.2), policy in §2.3; the emergency return to Android is ignored with the cover closed.

### 2.10 Touch (NT36532E THP)

Raw frames are processed in userspace (`piano-thp.service`, started by udev, restart back-off from 1 s to 30 s).
Power-relevant kernel behaviour: the THP stream starts after panel *enable* (a TDDI needs display timing to scan) and stops
at disable, asynchronously (v0.4.0); the touch firmware gets the external-power state (vendor host command 53/51) on every
power-supply online change, only while the stream is awake, resent after a firmware reload, charge pumps excluded, supply
queries drained on suspend (K:bd04ba0e1289, K:722ec8462836); the event page is selected once per stream (K:ba725ea02c92);
the GPI DMA latency limit (§2.7).

### 2.11 Ambient light and automatic brightness

SSC sensors through hexagonrpcd, libssc and a patched iio-sensor-proxy. v1.1.1 fixed light readings stopping after the
sensor was switched off and on (serialized SSC light polling, D:b19b586). The GSD patch `ambient-zero.patch` (D:409490d)
accepts 0 lux as valid darkness once a normalization baseline exists. Automatic brightness was drafted for v1.2.0 and then
dropped from the final notes and README (D:7fe59cc).

### 2.12 USB-C

DIO4485 SBU/orientation switch on the fsa4480 driver with I/O errors propagated, pmic_glink_altmode DP assignments with route
failures honoured, the stock DP rate limit (pinned from 09-30: D:3b719b4, D:bbadcee, D:cfe2480); USB2/3 dual-role dwc3
connected to UCSI (v0.5.0); the QMP combo PHY's cached mode tracked as committed or not (v0.6.0); v1.1.0: dwc3 mode switch,
the UTMI clock as PIPE clock in DP-only modes and a role lock shared with Type-C clock updates, so USB works after a DP
unplug (K:68d28640e1d1, K:9834284b801f). Powered docks charge and host at once. Open: a plain OTG adapter after an OTG
device may not be detected (since v0.5.0); hub resume (#21).

### 2.13 GENI and PDC interrupt work around sleep

GENI UART force suspend in v0.8.0; IRQ masked across the full transition and drained before shutdown in v1.2.0 (§2.1). PDC:
"on SM8750, three reversible pairs on Xiaomi Pad 8 Pro showed GPIO80 and GPIO95 pending at CX restoration with the original
level bits"; setting the PDC-to-GIC SPI interface to edge let the short windows reach the RTC alarm, but a longer one still
woke from GPIO95 after ~375 s (K:cd3e49bae2b5, K:81bc110b53c3). The series was **withdrawn** in v1.2.0 because the stock
DT's PDC resource (0x64 bytes) does not cover the needed SPI configuration word and no verified register map exists: "the
keyboard wake issue remains unresolved" (K:ff6cf0aafb8f).

### 2.14 DSP robustness (charger, audio and sensors share the ADSP)

- GLINK: the ADSP's first QRTR HELLO could arrive before the endpoint callback existed and was dropped; 1 cold boot in 26
  then had every channel open but no PDR indication, no charger, no Type-C, no audio and no sensors (K:235ab34e2a6e, v0.6.2).
- Sensor registry: a wedged DSP-written registry made the ADSP crash-loop about 8 times per minute through `sns_watchdog`
  (#3); fixed by implementing the DSP's rename in the FastRPC file server and quarantining a registry that keeps crashing
  (v0.6.1). Every ADSP restart resets the charger, and the driver re-applies everything per service generation.

## 3. Lessons for the macOS port (priority order)

### 3.1 PianoPower level 2: the input held at 0 mA (P0)

Facts to steer by: Linux never recorded 0 mA input with the DSP running (§2.5: ≥ 394 mA with no host write at all), never
wrote 0x2019 or 0x2012, and charged on wall chargers with ICL and FCC writes only (v0.3.0). Our G/H images test the
input-suspend hypothesis; the history adds these checks, cheapest first:

1. **Remove the confounders Ubuntu's tests avoided.** Run at 40–90 % on both packs (our 0 mA run was at 98 %, and 0x20004
   reads 0 when the firmware considers charging done). Run on a wall DCP or PD charger as well as the Mac's port: Ubuntu has
   no SDP/CDP result at all; its accepted paths are wall chargers and powered docks. Judge by the gauges' summed current.
2. **Log the firmware's own view of the port** in the `diag:` line: 0x200d OTG boost status, 0x200e OTG gate status, 0x21015
   Type-C mode, 0x21022 sink/source mode, 0x21002 verify_process (non-zero = adapter authentication state, the #20 degraded
   case), 0x20001 online, the 0x2007 float readback. A DSP that believes it is sourcing, or is stuck in authentication,
   would explain a live 5.2 V bus with no input current.
3. **Send the stock board configuration** once per DSP generation, after charger_pd is up and before any policy write,
   retried until all three succeed: 0x201c = 1 byte (charge pump present), 0x201a = the 16-byte piano OTG configuration,
   0x201b = 0x708. Allowlist exactly these values. It is the only charger-side configuration Linux sends that we do not,
   and #18's recovery by rebind re-sent it.
4. **Check the PAN encoding.** Our run logged "0 received, 0 acked, enable sent". Linux sends the enable as opcode 0x15
   with cmd 0x10 (not as opcode 0x10) and acks with opcode 0x15, cmd 0x11, arg = port; notifications come as opcode 0x16.
   Send it after charger_pd's PDR "up"; if PianoPower does not register a servreg listener for `msm/adsp/charger_pd` and
   acknowledge its state indications, add that too (Linux gates every client on it).
5. Only then the H image (0x2019 = 0). If it works, record it as a native-only difference: Linux never needed it.
6. Watch for ADSP restarts during the run (SMP2P error/ready bits, the crash reason): natively nobody serves the sensor
   framework's FastRPC files, #3 shows the sensor watchdog can take the whole ADSP down, and each restart puts the charger
   back to 100 mA and 0.
7. Keep in mind the boot-chain difference: Ubuntu always booted through the stock XBL/UEFI charger stage and started the
   ADSP during kernel boot; we boot a stubbed QcomChargerDxeLA and start the DSP 30 s later.

Beyond the gate:
- Treat a 0x2008 readback as "requested", not "in force" (K:1e4a0fc89fee).
- Correct `docs/drivers/charging.md`, which says the DSP keeps a 4350 mV float: the firmware default is 4400 mV (0x2007 reads
  2·mV − 3600); stock full is 4480 mV, 4060 mV from 48 °C. Keep float writes off until the telemetry is solid.
- On driver stop or failure **leave the last good limit in place** instead of restoring 100 mA: in Linux the hand-back to
  the DSP defaults is what drained the tablet when the unit was stopped. Restore the defaults only on detach.
- Before any sleep work: hand charging over as v1.1.0 does (keep the source type's 5 V limits, stop the CP, return to 5 V)
  and re-run the full sync after resume; #18 shows the firmware's port state can wedge until the host re-sends everything.
- If a charge limit is ever offered: stock's model is to stay awake while protected wired charging is below the limit,
  because the host's SOC polling cannot run asleep (K:80eeae96d596).
- MiPPS/PPS later: request DFP before reading the adapter SVID (#20) and clear verify_process on every exit path.

### 3.2 Suspend and idle (P1)

- Keep macOS sleep off (`sleep-disabled`) until the native equivalent of the v0.8.0 list holds: every block drops its RPMh
  and clock votes (LPASS idle votes, GMU/GX, UFS lane clocks, PCIe in D3 or off, GENI UARTs, Wi-Fi without the RF clock
  vote, dwc3 glue clocks). Ubuntu's target is s2idle, not PSCI SYSTEM_SUSPEND: all CPUs in the domain idle state with the
  wake IRQs armed reach CX collapse and DDR self-refresh. Model a native "sleep" the same way.
- **The LLCC island slice must be reserved before LPASS votes are released in sleep** (K:1d5861e2ee37), else every suspend
  resets the SoC. Natively no LLCC driver programs the slices, so the boot chain's table applies; read SCID 12's
  configuration before sleep experiments.
- Wake: RTC, power key, keyboard status pin TLMM 95 (only while blanked, not with the cover closed), hall TLMM 80 (opening
  only). Expect early wakes from 80/95 (§2.13) and count wakes per source in every test.
- Never touch dwc3 QSCRATCH with the glue clocks off (the unplug-in-sleep reset, K:7dffd78b342d); mask GENI IRQs before
  their registers lose power; mask the tsens lower thresholds across sleep (K:08e750858038).
- Test like `tools/piano-suspend-check.py`: boot identity, BOOTTIME − MONOTONIC, per-source wake IRQ counts, then peripherals
  and charging after resume; measure sleep power with the USB cable out (the HS PHY cannot reach L2 with a host attached).

### 3.3 Default power policy (P1, cheap)

Adopt Ubuntu's defaults as the drivers arrive: panel at 120 Hz; power key = display off and lock; system sleep after 5 min
idle on battery, none on external power; cover closed on external power = display off and lock, on battery = sleep; the
Balanced caps (policy0 3.5328 GHz, policy6 4.0896 GHz, boost off) and GPU 160–1100 MHz. Power Saver (1.7856 / 1.9584 GHz,
GPU ≤ 342 MHz) is a ready low-power setting for long native sessions.

### 3.4 CPU idle, DVFS and thermal (P1)

- Native XNU has no thermal policy yet: until it does, cap the CPUs at the Balanced values or lower. When implementing,
  use the stock numbers: 105 °C passive with 5 °C hysteresis on the CPU hotspot sensors, 10 ms polling while passive
  (heating is ~200 °C/s), 120 °C hot, 125 °C critical.
- Deep CPU idle costs up to 4.4 ms to exit; any DMA-driven peripheral that stalls in the domain state (GPI SPI, §2.7) needs
  a latency constraint while its transfer runs. PianoTouch is the first such client.

### 3.5 PianoA830 GPU idle (P2)

Floor 160 MHz (our runs already use 160 MHz). Never vote GMXC at retention with GX on (use the first level above, 50). Drop
the boot bandwidth vote once the GMU votes, mask the HFI IRQ before booting the GMU, and make sure the GMU/GX votes fall to
zero when idle, or CX cannot collapse (v0.8.0). The DRV-2 owner lock (`docs/drivers/charging.md` §5c) is the right pattern
for sharing RSC TCS slots with PianoPower.

### 3.6 Backlight KTZ8866 and display off (P2)

Two chips: write both (primary, then secondary), keep the boot brightness, read-modify-write the sink register, **never pull
HWEN low** (it removes the LCD bias the panel runs on). For display off: stop the video pipeline before the timing engine,
do not reset the shared controller while it scans, and deliver blank/unblank to the keyboard and touch drivers (§2.9, §2.10).

### 3.7 PianoKeyboard: WN8030, touchpad, hall gpio80 (P2)

Mirror the panel-blank sequence (0x25 = 0 and the sleep line; on unblank release it, wait for TLMM 97 for ≤ 100 ms, drain,
0x25 = 1, restore LEDs). No periodic keep-awake. Wake on the status pin (TLMM 95) only, never on the data IRQ. Ignore
keyboard wake while the hall reads closed; the hall wakes on opening only. Touchpad: detect it and retry after a failed
creation. Publish attach/detach as a tablet-mode state for rotation policy.

### 3.8 PianoTouch NT36532E (P2)

Start scanning after panel enable, stop at disable, restart asynchronously; hold a CPU latency limit during each DMA read;
select the event page once per stream; send the charger state (host command 53/51) when the USB input goes online or
offline, which PianoPower can publish.

### 3.9 USB-C (P3)

Handle DIO4485 orientation/SBU and DP route errors instead of ignoring them; keep role-switch register writes away from
suspended glue clocks; a USB link to a host keeps the HS PHY out of L2, so judge sleep power with the cable out.

## 4. Still open in Ubuntu at v1.2.0 (no answers to expect there)

Keyboard/hall early wakes (PDC), hub and DP resume (#21), OTG adapter re-detection, DSP sleep-state notification (off),
fast-charge power below stock, no published battery-life or suspend-drain numbers, no computer-port charging result.
