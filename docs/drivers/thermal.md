# Thermal and CPU frequency: TSENS, protection, SCMI performance: PianoThermal

Status: code, 2026-11-11. `package/PianoThermal` is built, host-tested (`thermal_host: all passed`; the sensor map checked against
the DT: 47 sensors) and linked into the native KC (kernel 0059: `com.unixnu.driver.PianoThermal: linked, ... 374 pointers`, none
from DroidVM), inert without `piano-thermal` / `piano-cpufreq`; **not run on hardware**. Template: `README.md`.

## 1. Purpose and gate

The native system had no temperature readout and no CPU frequency control. The GPU login screen already reached 57 C (PianoA830's own
reading; its DCVS holds 160 MHz from 70 C), the tablet is used daily in a charging stand, and the battery drains fast. PianoThermal
gives (a) the SoC's temperatures on the console and in the IORegistry, (b) a conservative protection policy, (c) the CPU clusters'
performance level over SCMI, a fixed "battery" level for an A/B test against the battery current, and a load-based governor with
Ubuntu's power-profile caps. Gates daily use and any session long enough to heat the SoC (plan R4); battery life (open question 1).

## 2. Hardware facts

Sources: DT = `out/inputs/piano-boot.dtb` (Ubuntu's running DT); UH = `docs/ubuntu-history/device.md` §2.6, §2.7, §3.4; TSV2 = Linux
`drivers/thermal/qcom/tsens-v2.c`, CPUCP = Linux `drivers/mailbox/qcom-cpucp-mbox.c` (Ubuntu's tree; GPL, as the specification
only); SCMI = Arm DEN0056; A830 = Adreno830 `Sm8750Tsens` (13T, SM8750); K90 = the REDMI K90 XNU port's `annibale_cpufreq.c`
(SM8750); pins in `package/PianoThermal/UPSTREAM`.

| Item | Value | Source | State |
|---|---|---|---|
| TSENS | four `qcom,sm8750-tsens` / `qcom,tsens-v2`: TM `0xc228000`/`0xc229000`/`0xc22a000`/`0xc22b000`, SROT `0xc222000`..`0xc225000` (0x1000 each), 15/7/16/9 sensors, IRQs "uplow"/"critical" (not used: polled) | DT | verified |
| Sensor map | tsens0: aoss0, cpu-0-{0..5}-{0,1} (1..12), cpuss-0-{0,1}; tsens1: aoss1, cpu-1-{0,1}-{0,1} (1..4), cpuss-1-{0,1}; tsens2: aoss2, gpuss0-7 (1..8), modem0-3, camera0-1, video; tsens3: aoss3, nsphvx0-2, nsphmx0-3, ddr. Cooling maps: cpu-0-* → cpu0's policy, cpu-1-* → cpu6's, gpuss* → the GPU | DT thermal-zones | verified (`check-thermal-map.py`) |
| Stock trips | the nine CPU hotspots (cpu-0-0-0, cpu-0-2-\*, cpu-0-4-\*, cpu-1-\*) passive 105 C / 5 C (polling 10 ms while passive), the seven others cpu-0-* 118 C / 8 C; hot 120, critical 125; gpuss* passive 105, critical 125; others hot 120, critical 125; PMIC temp-alarms passive 95, hot 115 | DT, UH | verified |
| Heating | one prime-core load: a zone 34.5 → 91 C in 0.26 s (~200 C/s); stock (4.32 GHz) holds these zones at a median 102.4 C, never above 105.5 C | UH | measured on Ubuntu |
| Readout | TM_Sn_STATUS `0xa0+4n`: [11:0] signed deci-C, VALID bit 21 (MIN..MAX status 16-20); thresholds `0x20+4n` (lower [11:0], upper [23:12]), critical `0x60+4n`; TRDY `0xe4`; SROT HW_VER `0x0`, CTRL `0x4` (EN bit 0, SENSOR_EN [18:3], temperature format bit 21) | TSV2 | status word verified on SM8750 (A830, K90); SROT/thresholds inferred |
| Access | Ubuntu's tsens driver reads both ranges at EL1 under the same Gunyah stage 2 | DT, UH | inferred |
| Board NTCs | 11 `generic-adc-thermal` sensors on the PMK8550 ADC5 gen3 (xo, msm, flash, wifi, display, backlight, conn2, battery, conn, quiet, charger1), monitoring only on Ubuntu | DT, UH | verified; **not read**: a conversion is an SPMI write to the PMIC (pack/charger temperatures: PianoPower) |
| CPU clusters | cpu0-5 (MPIDR Aff1 0..5, `E`), cpu6-7 (Aff2 1, `P`); `power-domains` "perf" = SCMI perf domain 0 / 1; no `qcom,cpufreq-hw` node, no CPU OPP table (the levels come from the firmware) | DT, `dts/piano.dts` | verified |
| SCMI | `arm,scmi`, `protocol@13` (perf); `mboxes` CPUCP channel 0 (tx), 2 (rx); `shmem` `0x17b4e000` + `0x200` (tx), `+0x200` (rx) | DT | verified |
| CPUCP mailbox | `qcom,sm8750-cpucp-mbox`: reg 0 rx `0x16430000`, reg 1 tx `0x17830000`; doorbell = tx + `0x100 + 8*ch + 4` = `0x17830104` (any value) | DT, CPUCP | verified (source); hardware-verified on K90 |
| Levels | K90: perf 4.0 with level indexing, "NCC0" 16 levels 384-3532.8 MHz, "NCC1" 16 levels 1017.6-4320 MHz, sustained 4089.6 MHz (above it: boost); Ubuntu's minima 384 / 1017.6 MHz | K90, UH | **needs-boot** on piano (`piano-cpufreq=1` logs them and the level the boot stages left) |
| Profiles | Ubuntu (schedutil): Power Saver cpu0-5 ≤ 1785.6, cpu6-7 ≤ 1958.4 MHz, GPU ≤ 342; Balanced 3532.8 / 4089.6 MHz (boost off), GPU ≤ 1100; Performance 3532.8 / 4320 (boost) | UH | verified (Ubuntu, on the device) |
| Votes | none: TSENS is always-on; SCMI perf is the CPUCP's; PianoThermal takes no GCC/RPMh lock | | verified (code) |

## 3. Ubuntu as the specification

`tsens-v2.c` + `tsens.c` (`get_temp_tsens_valid`: VALID retry, `tsens_hw_to_mC`: sign-extend 12 bits, x100 for v2), the zones'
step_wise governor (one cooling step per polling interval at/over the trip, back under trip - hysteresis); `qcom-spmi-adc5-gen3.c`
+ `generic-adc-thermal` for the NTCs (not taken over); `scmi-cpufreq.c`, `arm_scmi/perf.c` and `shmem.c` for the perf domains
(DESCRIBE_LEVELS paging, level indexing, polled transfers, levels above sustained = boost); `qcom-cpucp-mbox.c` for the doorbell;
TuneD profiles for the caps (§2).

## 4. UniXNU template to build on

No UniXNU kext reads TSENS or drives SCMI for piano. Taken (facts, no code): A830's status decode (PianoA830 uses it for the GPU
sensors), K90's SCMI addresses and polled transport on the same SoC, UniXNU-sheng `CpuFreqProbe.cpp`'s mode ladder and governor
shape (SM8550 EPSS: not piano's registers). PianoThermal is new code (BSD-2-Clause-Patent), portable parts host-tested: `Tsens.h`
(map, decode, poll), `Policy.h` (state machine), `Cluster.h` (a cluster against its trips), `Scmi.h` (transport, perf codec),
`Governor.h` (fixed/governor/thermal limit).

## 5. IOKit family / ABI

`PianoThermal : IOService` on `IOPlatformExpertDevice` (personality in `tooling/k27/kext/PianoThermal.json`), one kernel thread.
Properties: `TemperaturesDeciC` (dict: cpu0/cpu1/gpu/nsp hottest), `SensorsDeciC` (data: int16 [4][16], -32768 = no reading),
`ThermalState`, `PeakDeciC`, `CpuFreq` (cluster0/1 MHz), `piano-thermal`, `piano-cpufreq`. GPU: at passive
`callPlatformFunction("piano-gpu-ceiling", khz)` on PianoA830's registered service (reversible, cleared below passive; no link
dependency; proposed in §10.5, answered kIOReturnUnsupported until then), else and at critical PianoA830's exported
`adreno830_dcvs_park()` (lowest level and bus for the rest of the boot); `adreno830_thermal_state()` (read). All are no-ops while
PianoA830 is inert. The GPU's own caps stay PianoA830's (its default 342 MHz = Power Saver's GPU maximum). Not used, on purpose: IOPMrootDomain's `kIOPMOverTemp` (an emergency *sleep*: no sleep natively),
`systemPowerEventOccurred(kIOPMThermalLevelWarningKey)` (the root domain's thermal warning, which makes it prefer sleep and treat full
wake as unsustainable), `shutdownSystem()` (kIOReturnUnsupported in k27); macOS thermal pressure is a userspace notification: no
clean KPI. The shutdown is the kernel's own halt path `reboot_kernel(RB_HALT)`: processes signalled, disks synced and unmounted,
IOKit's shutdown notifications (IOPMrootDomain `handlePlatformHaltRestart`), PSCI SYSTEM_OFF (k27 0053).
CPU load: XNU's per-processor idle time from `processor_info(cpu_to_processor(i), PROCESSOR_CPU_LOAD_INFO)` (recount, 1/hz s ticks:
±10 ms per 100 ms tick), busy = 1 - idle / wall, the busiest core per cluster. Power source: the `IOPMPowerSource`'s
`ExternalConnected` (PianoPower with `piano-power-ps=1`), unknown = battery caps. Polling tiers (`Cluster.h` pollTier): base 1 s
(the governor's 100 ms tick when it runs); 250 ms once the hottest sensor reaches 70 C; 10 ms (Linux's passive polling) while any
sensor is within 10 C of its trip, a cluster is thermally limited, or a cluster runs above 2.5 GHz (where a prime-core load heats
at ~200 C/s). The upper-threshold interrupt replaces the warm tier later (§10.4). The idle states (PSCI cpuidle) are another
driver's: not touched.
**Suspend hooks** (for the suspend driver; docs/power.md's contract when it lands): `int piano_thermal_suspend_prepare(void)` masks
every sensor's lower-threshold interrupt (`TM_UPPER_LOWER_INT_MASK` 0x10 bits 0..n-1, clears pulsed 1→0 at 0x0c; Linux
08e750858038) and, while PianoThermal does not service the uplow interrupt itself, the upper ones (bits 16..) too; critical
untouched; returns the controllers masked, -1 if already prepared. `void piano_thermal_suspend_finish(void)` restores the masks
found. Also as `callPlatformFunction("piano-thermal-suspend-prepare", int *result)` / `"-finish"` on the PianoThermal service. They
map the controllers themselves (work without `piano-thermal`) and are the kext's only TSENS writes.

## 6. Boot args

| Argument | Default | Effect |
|---|---|---|
| `piano-thermal=N` | 0 | 0 off (probe declines, nothing mapped); 1 readout: loads only; 2 readout + protection |
| `piano-thermal-shutdown=1` | 0 | at level 2's critical state: halt (`reboot_kernel(RB_HALT)`); 0 logs only |
| `piano-thermal-warm=<C>` / `-margin=<C>` | 85 / 10 | warm (a log line); critical = stock critical (125) - margin, held 1 s |
| `piano-thermal-poll-ms` / `-log-s` / `-delay` | 1000 / 30 / 0 | base poll and policy period; report period; start delay (s) |
| `piano-thermal-warm-poll=<C>` / `-warm-ms` | 70 / 250 | the warm tier: from this hottest reading, poll this often |
| `piano-thermal-fast-ms` / `-fast-mhz` | 10 / 2500 | the fast tier (near a trip, limited, or a cluster above this clock); also the thermal limit's step |
| `piano-thermal-gpu-mhz` | 160 | the reversible GPU ceiling at passive (when PianoA830 offers it) |
| `piano-thermal-clock=0` / `-srot=0` | 1 / 1 | skip the per-core clock measurement / the SROT reads |
| `piano-cpufreq=N` | 0 | 1 report (SCMI messages, nothing set); 2 fixed level; 3 governor. Implies `piano-thermal=1` |
| `piano-cpufreq-cap=<MHz>`, `-cap0`, `-cap1` | 1785 / 1958 (Power Saver) | mode 2: each cluster at the fastest level ≤ cap (and ≤ sustained); mode 3: a hard ceiling when given. A cap admits its whole MHz (1785 admits 1785.6) |
| `piano-cpufreq-bat0/1`, `-ac0/1` | 1785/1958, 3532/4089 | mode 3 ceilings on battery or power unknown (Power Saver) / on external power (Balanced) |
| `piano-cpufreq-hispeed0/1`, `-min0/1` | 1500/1800, 0 | mode 3: level at ≥ 90 % busy; floor |
| `piano-cpufreq-tick-ms` | 100 | mode 3 load decision period (20..1000) |
| `piano-cpufreq-hot` / `-release` / `-cut` | stock | cluster thermal limit (modes 2, 3): default each cpu-* zone's passive trip / trip - hysteresis / 125 - margin; given: absolute C for all |

## 7. Kernel archive / component

Native kext `com.unixnu.driver.PianoThermal`, listed in `configs/k27-native-kexts.txt` after PianoA830 (named as a library: its GPU
hooks). Build: `tooling/k27/build-native-piano-thermal.sh` (host tests, the DT map check when `out/inputs/piano-boot.dtb` and dtc
exist, objects, import check); `build-native-image.sh` maps it. Breadcrumbs: `piano_thermal_state`, `piano_thermal_hottest_deci`.

## 8. Hardware test plan

All RAM boots with the cable, owner present. Every level is off by default, so a bad image is fixed by its arguments.

1. **Level 1** (read only):
   `PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=300 PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-thermal=1" tooling/k27/build-native-image.sh`
   then `tooling/native/native-loop.sh out/ram/boot-piano-macos-ram-A-bundled.img`. Expected (numbers are examples):
   ```
   PianoThermal: start: piano-thermal=1 piano-cpufreq=0 shutdown=0 poll 1000 ms, log every 30 s, warm 85 C, critical margin 10 C, ...
   PianoThermal: level 1: readout only: loads, nothing written
   PianoThermal: tsens0 cpu0 @0xc228000: version 2.x.y (0x...), ctrl 0x...: enabled, 15 sensors enabled (DT 15, matches), result temperature
   PianoThermal: tsens0 cpu0: TM int-en 0x..., up/low status 0x0, critical status 0x0, trdy 0x1; hardware thresholds: critical ..., upper ...
   PianoThermal: scmi: tx channel @0x17b4e000 (read only): status 0x1 (free), flags 0x0, length ..., last header 0x...
   PianoThermal: running: the sensors every 1000 ms (every 20 ms near a trip), the policy every 1000 ms, a report every 30 s
   PianoThermal: [0s] cpu0 41.2 C (s7 cpu-0-3-0), cpu1 43.0 C (s3 cpu-1-1-0), gpu 45.1 C (s4 gpuss3), nsp 39.8 C (s8 ddr); peak 43.0 C (cpu1 s3); state normal
   PianoThermal: [0s] reads: 1 polls, 47 loads, 0 without VALID, 0 implausible, 0 stale stand-ins
   PianoThermal: [0s] clock (dependent adds, MHz): cpu0/0 1017 ... cpu6/1 4320 cpu7/1 4320
   PianoThermal: [30s] ... ; a830 52.0 C          (once PianoA830's DCVS runs)
   ```
   Pass: four controllers enabled with the DT's sensor counts, plausible readings that rise with load, a830's value close to the
   gpu line. The clock line answers "did the boot stages leave the prime cores fast?" without any message.
2. **piano-cpufreq=1** (report): the same command with `piano-thermal=1 piano-cpufreq=1`. Expected: `cpufreq: SCMI performance
   protocol 4.0, 2 domains`, per domain `"NCC0"... 16 levels (MHz): ...` and `at boot N MHz (level i of 16[, at or above sustained:
   the boot stages left it fast])`, every 30 s `cpufreq d0 NCC0: now N MHz (boot N MHz); ...` and `scmi: N messages, 0 failed`.
3. **A/B for the battery** (on battery, screen on, idle desktop, 15 min each; PianoPower's `[30s] battery: ... mA` is the measure):
   ```sh
   PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=300 PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-thermal=1" tooling/k27/build-native-image.sh
   for m in "report:piano-cpufreq=1" "saver:piano-cpufreq=2" "gov:piano-cpufreq=3"; do
     PIANO_RESET_AFTER=900 PIANO_EXTRA_ARGS="piano-gpu=2 piano-gpu-delay=20 panic_on_crash_disabled=1 piano-usb=2 piano-power=1 piano-thermal=2 ${m#*:}" \
       python3 tooling/bootimg/build-ram-images.py --fd out/fd/piano-uefi-A-bundled.fd --fd-name A-bundled --kc out/k27/kc-k27-native.bin \
       --trust out/turnip-a830/stage/kernel.macho.trustcache --seed out/turnip-a830/stage/scratch-data.img --adt out/adt/piano-k27.adt \
       --utc "$(date +%s)" --out "out/ram-cpufreq/${m%%:*}"
   done
   ```
   Expected in `saver`: `cpufreq: domain 0: fixed at the fastest level <= 1785 MHz (and <= sustained): 1785 MHz; thermal limit: stock
   trips ...` (domain 1: 1958), the 30 s lines `now 1785 MHz`; in `gov`: `governor every 100 ms ...`, `power source: on battery` (or
   `power unknown (battery caps)`), residency mostly at the slowest level when idle (`384 MHz 9x%`). Compare the mean battery
   current of the three sessions; a lower cap (`piano-cpufreq-cap=1000`) is a fourth point.
4. **Level 2** (protection) once level 1 shows sane readings: `piano-thermal=2`. Nothing visible below 85 C. Lines: `state normal ->
   warm: ...`, `polling every 10 ms: a sensor is within 10 C of its trip`, `[30s] polling: base ... ms, warm ... ms, fast ... ms`,
   `PASSIVE: ... GPU capped at 160 MHz ...` (or `GPU parked ...; PianoA830 has no reversible ceiling`), `critical threshold met ...`,
   `CRITICAL: ... NOT shutting down (log only)`. `piano-thermal-shutdown=1` only after a session showed no wild readings.
5. **Suspend hooks** (with the suspend driver's first s2idle test): `suspend prepare: lower and upper threshold interrupts masked on
   4 controllers: tsens0 0x...->0x7fff7fff tsens1 ...->0x7f007f ...`, after resume `suspend finish: ... back on 4 controllers`. A
   sleep that still ends at once with these lines points at another wake source; fewer than 4 controllers: a mapping failed.

Failure meanings: `NOT MAPPED` (no mapping: nothing read from that controller); `NOT ENABLED` / `DIFFERS` (the boot stages left
TSENS off or partly configured: readings will lack VALID); `ADC CODES` (result format is codes: the decode is wrong); many `without
VALID` or `implausible` (stale controller); `NO READING ... protection is blind` (no usable sensor for 5 s); `scmi: ... status 0x0
(BUSY)` at level 1 (a message left in flight: `piano-cpufreq` will time out); `cpufreq: SCMI performance protocol: no answer (-100
channel never free | -101 no answer)` (the CPUCP does not serve the channel natively: nothing changed); `level ... refused` (the
firmware's own limits; after 20 the domain is left alone); a hang right after `running:` (a core that never takes the bound
thread of the clock measurement: boot with `piano-thermal-clock=0`).

## 9. Fallback

Without the arguments nothing is mapped. The hardware's own protection stays: TSENS critical thresholds (logged at level 1) and the
CPUCP's limits management; PianoA830's own GPU rule (160 MHz from 70 C) is independent of this kext. Without `piano-cpufreq` the
clusters stay at the level the boot stages left (open question 1).

## 10. Open questions

1. Which levels did the boot stages leave? (`piano-cpufreq=1`; NCC1 at 4.32 GHz would be a battery drain modes 2/3 remove.)
2. Does the CPUCP serve SCMI natively under Gunyah at EL1 as on K90? (`piano-cpufreq=1`.)
3. Are the TSENS hardware thresholds (critical / upper) programmed by the boot stages? (level 1's threshold line.)
4. **The upper-threshold interrupt** (when the GIC/PDC helper lands): per CPU sensor `TM_Sn_UPPER_LOWER_THRESHOLD` = lower 70 C,
   upper 85 C (`encodeUpperLower`, Tsens.h), their upper bits unmasked in 0x10, `TM_INT_EN` = 7; the "uplow" lines are SPI 771-774
   (DT `0x303`-`0x306`, level high), "critical" SPI 484-487. The handler masks the sensor's upper bit, pulses its clear and wakes the
   thread, which then polls at the fast/warm tiers until every sensor is under 70 C and re-arms. The base poll can then go to 5 s.
   `kServicesUplow` (PianoThermal.cpp) becomes true, so the suspend hook keeps upper armed as Linux does.
5. **PianoA830 patch proposal: a reversible DCVS ceiling** (owner: the GPU driver; forward as is). Behaviour without a caller:
   identical (the ceiling is 0 = none).
   - `A830Dcvs.h`, under `ADRENO830_MACOS`, marked "UniXNU-piano": `extern "C" void adreno830_dcvs_set_ceiling(uint32_t khz);`
     (0 clears; lock-free, any thread) and in `A830Dcvs`: `uint32_t externalKhz_ = 0;` (atomic), `uint8_t externalCeiling_ = 0;`,
     `void setExternalCeiling(uint32_t khz)` (store, then `env_.wake(env_.context)`).
   - `A830Dcvs.cpp`: in `serviceTick`, next to the park block: load `externalKhz_`; the wanted level is
     `khz ? max(levelAtMost(khz), 1) : 0`; when it differs from `externalCeiling_`, store it and `updateWakeTargets()`. In `ceiling()`:
     `c = min8(knobCeiling_, thermalCeiling_); if (externalCeiling_) c = min8(c, externalCeiling_);` (the vote path already clamps
     to `ceiling()`, so the next window drops at once). Optionally an `A830ThermalExternal` flag in `adreno830_thermal_state` and an
     "X" on the DCVS row.
   - `board/PianoA830.cpp` `callPlatformFunction`: `"piano-gpu-ceiling"` with `param1 = (void *)(uintptr_t)khz` →
     `adreno830_dcvs_set_ceiling(khz)`, `kIOReturnSuccess` (needs `registerService`, done at `piano-gpu=2`).
   - `UPSTREAM`: the two files' new sha256 and the change's line. PianoThermal needs no change: it already asks for it (falls back
     to the park while the answer is kIOReturnUnsupported) and clears it below passive.
6. The PMIC NTCs and temp-alarm stages need SPMI reads/conversions: later (PianoPower's SPMI code).
7. Does `reboot_kernel(RB_HALT)` power the tablet off natively (PSCI SYSTEM_OFF under Gunyah)? (one deliberate test, owner present.)
8. Is ±10 ms idle accounting enough for the governor, or does it need recount's mach-time idle (a k27 helper)? (the `gov` session's
   residency and the touch feel.)

## 11. Licence note

New code, BSD-2-Clause-Patent (`package/PianoThermal/LICENSE`). Linux (GPL) and the K90 port (APSL) were read for facts only; no code
was copied. The SCMI layouts are from Arm's public specification.
