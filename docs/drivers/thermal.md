# Thermal and CPU frequency: TSENS, protection, SCMI performance: PianoThermal

Status: code, 2026-11-11. `package/PianoThermal` is built, host-tested (`thermal_host: all passed`; the sensor map checked against
the DT: 47 sensors) and linked into the native KC (kernel 0059: `com.unixnu.driver.PianoThermal: linked, ... 374 pointers`, none
from outside this repository), inert without `piano-thermal` / `piano-cpufreq`; **not run on hardware**. Template: `README.md`. B6, the eleven
PMIC board thermistors (battery, charger, connectors, skin, ...) and `piano_thermal_board_temps()` for charging safety: §12
(`piano-thermal-adc=1`, host-tested, linked, not run). 2026-11-11 (host-tested, linked, not run): PianoPolicy's
`"piano-thermal-profile"` (§5, `docs/policy.md` §5.2) and the power-tree join for system sleep (§5, `docs/power.md` §4).
2026-11-11 (host-tested, linked, not run): the TSENS upper/lower threshold interrupts behind `piano-thermal-irq=1` (§13; default
off; native SPI delivery under Gunyah unproven, the batched test §13.8 settles it).

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
| TSENS | four `qcom,sm8750-tsens` / `qcom,tsens-v2`: TM `0xc228000`/`0xc229000`/`0xc22a000`/`0xc22b000`, SROT `0xc222000`..`0xc225000` (0x1000 each), 15/7/16/9 sensors, IRQs "uplow" SPI 771-774 / "critical" SPI 484-487 (level high, the GIC; uplow only with `piano-thermal-irq=1`, §13; critical never) | DT | verified (`check-tsens-irq.py`) |
| Sensor map | tsens0: aoss0, cpu-0-{0..5}-{0,1} (1..12), cpuss-0-{0,1}; tsens1: aoss1, cpu-1-{0,1}-{0,1} (1..4), cpuss-1-{0,1}; tsens2: aoss2, gpuss0-7 (1..8), modem0-3, camera0-1, video; tsens3: aoss3, nsphvx0-2, nsphmx0-3, ddr. Cooling maps: cpu-0-* → cpu0's policy, cpu-1-* → cpu6's, gpuss* → the GPU | DT thermal-zones | verified (`check-thermal-map.py`) |
| Stock trips | the nine CPU hotspots (cpu-0-0-0, cpu-0-2-\*, cpu-0-4-\*, cpu-1-\*) passive 105 C / 5 C (polling 10 ms while passive), the seven others cpu-0-* 118 C / 8 C; hot 120, critical 125; gpuss* passive 105, critical 125; others hot 120, critical 125; PMIC temp-alarms passive 95, hot 115 | DT, UH | verified |
| Heating | one prime-core load: a zone 34.5 → 91 C in 0.26 s (~200 C/s); stock (4.32 GHz) holds these zones at a median 102.4 C, never above 105.5 C | UH | measured on Ubuntu |
| Readout | TM_Sn_STATUS `0xa0+4n`: [11:0] signed deci-C, VALID bit 21 (MIN..MAX status 16-20); thresholds `0x20+4n` (lower [11:0], upper [23:12]), critical `0x60+4n`; TRDY `0xe4`; SROT HW_VER `0x0`, CTRL `0x4` (EN bit 0, SENSOR_EN [18:3], temperature format bit 21) | TSV2 | status word verified on SM8750 (A830, K90); SROT/thresholds inferred |
| Access | Ubuntu's tsens driver reads both ranges at EL1 under the same Gunyah stage 2 | DT, UH | inferred |
| Board NTCs | 11 `generic-adc-thermal` sensors on the PMK8550 ADC5 gen3 (xo, msm, flash, wifi, display, backlight, conn2, battery, conn, quiet, charger1), monitoring only on Ubuntu | DT, UH | verified; read by B6 with `piano-thermal-adc=1` (§12: a conversion is a write to the ADC's SDAM) |
| CPU clusters | cpu0-5 (MPIDR Aff1 0..5, `E`), cpu6-7 (Aff2 1, `P`); `power-domains` "perf" = SCMI perf domain 0 / 1; no `qcom,cpufreq-hw` node, no CPU OPP table (the levels come from the firmware) | DT, `dts/piano.dts` | verified |
| SCMI | `arm,scmi`, `protocol@13` (perf); `mboxes` CPUCP channel 0 (tx), 2 (rx); `shmem` `0x17b4e000` + `0x200` (tx), `+0x200` (rx) | DT | verified |
| CPUCP mailbox | `qcom,sm8750-cpucp-mbox`: reg 0 rx `0x16430000`, reg 1 tx `0x17830000`; doorbell = tx + `0x100 + 8*ch + 4` = `0x17830104` (any value) | DT, CPUCP | verified (source); hardware-verified on K90 |
| Levels | K90: perf 4.0 with level indexing, "NCC0" 16 levels 384-3532.8 MHz, "NCC1" 16 levels 1017.6-4320 MHz, sustained 4089.6 MHz (above it: boost); Ubuntu's minima 384 / 1017.6 MHz | K90, UH | **needs-boot** on piano (`piano-cpufreq=1` logs them and the level the boot stages left) |
| Profiles | Ubuntu (schedutil): Power Saver cpu0-5 ≤ 1785.6, cpu6-7 ≤ 1958.4 MHz, GPU ≤ 342; Balanced 3532.8 / 4089.6 MHz (boost off), GPU ≤ 1100; Performance 3532.8 / 4320 (boost) | UH | verified (Ubuntu, on the device) |
| Votes | none: TSENS is always-on; SCMI perf is the CPUCP's; PianoThermal takes no GCC/RPMh lock | | verified (code) |

## 3. Ubuntu as the specification

`tsens-v2.c` + `tsens.c` (`get_temp_tsens_valid`: VALID retry, `tsens_hw_to_mC`: sign-extend 12 bits, x100 for v2), the zones'
step_wise governor (one cooling step per polling interval at/over the trip, back under trip - hysteresis); `qcom-spmi-adc5-gen3.c`
+ `qcom-vadc-common.c` for the NTCs (B6, §12: the conversion protocol and the codes; the curve is derived, not taken);
`scmi-cpufreq.c`, `arm_scmi/perf.c` and `shmem.c` for the perf domains (DESCRIBE_LEVELS paging, level indexing, polled transfers,
levels above sustained = boost); `qcom-cpucp-mbox.c` for the doorbell; TuneD profiles for the caps (§2).

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
dependency; §10.5, in PianoA830 since 2026-11-11), else and at critical PianoA830's exported
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
at ~200 C/s). With `piano-thermal-irq=1` the upper-threshold interrupt stands in for the warm tier and the fast-clock rule while
every sensor is under 85 C, and the base relaxes to 5 s (§13.4). The idle states (PSCI cpuidle) are another driver's: not touched.
**Suspend hooks** (for the suspend driver; docs/power.md's contract when it lands): `int piano_thermal_suspend_prepare(void)` masks
every sensor's lower-threshold interrupt (`TM_UPPER_LOWER_INT_MASK` 0x10 bits 0..n-1, clears pulsed 1→0 at 0x0c; Linux
08e750858038) and, while PianoThermal does not service the uplow interrupt itself, the upper ones (bits 16..) too; critical
untouched; returns the controllers masked, -1 if already prepared. `void piano_thermal_suspend_finish(void)` restores the masks
found. Also as `callPlatformFunction("piano-thermal-suspend-prepare", int *result)` / `"-finish"` on the PianoThermal service. They
map the controllers themselves (work without `piano-thermal`) and are the kext's only TSENS writes.
**System sleep** (2026-11-11; `docs/power.md` §4, `Governor.h` SleepPlan, host-tested): PianoThermal is in the power tree
(`pianopower::join` in `start`). `setPowerState(off)` parks the thread at the top of a poll (no SCMI request, no TSENS read in
flight; the clusters stay at their level, nothing to power), and with `piano-thermal-sleep-mask=1` calls
`piano_thermal_suspend_prepare()`; `on` calls `piano_thermal_suspend_finish()` for what that off masked (not when another caller
had prepared first) and runs the thread again: the first zone update is at once, the power source is read again at the next CPU
tick. Default `piano-thermal-sleep-mask=0`: the threshold interrupts are left as found, since no TSENS interrupt is taken natively
(its SPI has no handler and stays disabled at the GIC); the contract's rule is "masked when interrupts are used". The ADC thread
(§12) is not parked (a conversion in flight at the freeze is a read error after the wake, §12.5). Lines: `system sleep: in the power
tree (0): the thread parks while suspended`, `[Ns] sleep: the thread is parked (no SCMI, no TSENS); threshold interrupts as they
are (piano-thermal-sleep-mask=0)`, `[Ns] wake N: polling again`. With `piano-thermal-irq=1` the interrupts are used, so a sleep
masks as with `piano-thermal-sleep-mask=1`, and while that path is live the hooks mask only the lower ones (Linux's rule, §13.6).
**The power profile** (2026-11-11; `docs/policy.md` §5.2, `Governor.h` Profile, host-tested): `callPlatformFunction(
"piano-thermal-profile", false, (void *)(uintptr_t)mode, ...)` on this service, any thread: 0 the caps by the power source (as
before), 1 Power Saver whatever the source (the `-bat0/-bat1` ceilings), 2 Balanced whatever the source (`-ac0/-ac1`); else
`kIOReturnBadArgument`, and `kIOReturnUnsupported` without CPU frequency control (`piano-cpufreq` < 2). An atomic the governor reads
every tick: the power source it hands `Governor::tick` / `enforce` becomes battery for 1, external for 2 (a lower ceiling applies at
once); the fixed level (mode 2) has no power caps and ignores it. One line per change (`[Ns] cpufreq: profile Power Saver
(PianoPolicy)`), the property `piano-thermal-profile` (0/1/2, with `piano-cpufreq` >= 2), and the 30 s report names it (`ceiling
1785 MHz (on power; profile Power Saver)`).
**Board thermistors** (B6, §12, `piano-thermal-adc`): a second kernel thread `piano-thermal-adc`; properties
`BoardTemperaturesDeciC` (dict: batt, charger, conn, conn2, msm, quiet, xo, wifi, display, backlight, flash; working channels only),
`BoardAdc` (dict: each channel `ok` / `no reading yet` / `off: <why>`), `BoardAdcState` (`off`, `starting`, `running`,
`stopped: <why>`), `piano-thermal-adc`; for charging safety `int piano_thermal_board_temps(struct piano_board_temps *)`
(`package/include/PianoBoardTemps.h`) and `callPlatformFunction("piano-thermal-board-temps", struct piano_board_temps *)` (§12.7).

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
| `piano-thermal-adc=N` | 0 | B6 (§12): 0 off (no SPMI access); 1 the eleven board thermistors on the PMIC ADC (writes: the ADC SDAM's conversion bytes); 2 the same with each round's raw codes. Implies `piano-thermal=1` |
| `piano-thermal-adc-s` | `-log-s` (30) | the ADC round period, 5..3600 s (the first round right after the start survey, after `-delay`) |
| `piano-thermal-sleep-mask=1` | 0 | a system sleep also masks the threshold interrupts (the suspend hooks, §5); 0: the thread is parked only |
| `piano-thermal-irq=1` | 0 | §13: the TSENS upper/lower threshold interrupts (SPI 771-774). Needs `piano-thermal` ≥ 1 (or an argument that implies it); a system sleep then masks as with `-sleep-mask=1` |
| `piano-thermal-irq-up=<C>` / `-down=<C>` | 85 / 70 | the window of every sensor: up 50..104, down 30..up - 5 |
| `piano-thermal-irq-poll-ms` | 5000 | the base poll at rest (1000..60000); 0: only the report's polls (`-log-s`); never the governor's tick |
| `piano-thermal-irq-hold-ms` | 1000 | the fast tier after an upper interrupt (100..10000) |
| `piano-thermal-irq-test=N` / `-test-s` | 0 / 120 | the synthetic test (§13.8): per controller the hottest sensor's upper threshold N tenths of a degree above its reading (1..200), for that many seconds (10..600), then its window back |

## 7. Kernel archive / component

Native kext `com.unixnu.driver.PianoThermal`, listed in `configs/k27-native-kexts.txt` after PianoA830 (named as a library: its GPU
hooks). Build: `tooling/k27/build-native-piano-thermal.sh` (host tests, the DT map check when `out/inputs/piano-boot.dtb` and dtc
exist, objects, import check); `build-native-image.sh` maps it. Breadcrumbs: `piano_thermal_state`, `piano_thermal_hottest_deci`.
B6 adds `tests/adc_host.cpp` and the ADC map check (`tooling/k27/check-adc-map.py`) to that script and `PianoThermalAdc.cpp` as the
kext's second object; it exports `piano_thermal_board_temps`. The uplow interrupt (§13) adds `package/common/tests/run.sh`,
`tests/uplow_host.cpp` and the interrupt table check (`tooling/k27/check-tsens-irq.py`) to that script and `PianoThermalIrq.cpp` as
the third object (it imports `armcpu_spi_attach` / `armcpu_spi_count` from the kernel).

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
5. **System sleep and the suspend hooks** (with the suspend driver's first s2idle test, `docs/power.md` §6 S1): each suspend
   `[Ns] sleep: the thread is parked (no SCMI, no TSENS); ...` and after the wake `[Ns] wake N: polling again`, the 30 s lines
   continuing. With `piano-thermal-sleep-mask=1` also `suspend prepare: lower and upper threshold interrupts masked on 4 controllers:
   tsens0 0x...->0x7fff7fff tsens1 ...->0x7f007f ...`, after the wake `suspend finish: ... back on 4 controllers`. A sleep that
   still ends at once with these lines points at another wake source; fewer than 4 controllers: a mapping failed.
5a. **The profile** (`docs/policy.md` §9 P4, PianoPolicy at level 2 with `piano-cpufreq=3`): on power `sysctl -w
   machdep.piano_policy_lpm=1` → `cpufreq: profile Power Saver (PianoPolicy)` and the next 30 s lines `ceiling 1785 MHz (on power;
   profile Power Saver)` (cluster 1: 1958); `=0` → `cpufreq: profile by the power source (PianoPolicy)`, `ceiling 3532 MHz (on
   power)`.
6. **Board thermistors** (B6): one RAM boot with `piano-thermal=1 piano-thermal-adc=1` (and `piano-power=1` for the gauges'
   temperature to compare); the procedure, the expected lines and what failures mean: §12.8.
7. **The uplow interrupt** (`piano-thermal-irq=1`): two RAM boots, the synthetic test and the governor; §13.8.

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
4. **The upper-threshold interrupt**: implemented 2026-11-11 behind `piano-thermal-irq=1` (§13), as proposed here (lower 70 C /
   upper 85 C on every sensor, the handler masking and waking the thread, today's tiers until every sensor is under 70 C, a 5 s
   base at rest, the suspend hook keeping upper armed), with the lower interrupt ending the hot phase, a proof at the start, and
   a fallback to polling. Open: does Gunyah deliver SPI 771-774 natively (§13.8)?
5. **PianoA830: a reversible DCVS ceiling** (implemented 2026-11-11 as proposed, host-tested in `test_a830idle`; the board logs
   `PianoA830: gpu ceiling N MHz (...)` and `gpu ceiling cleared`, the first 8 changes then every 10th, 100th, ...; a ceiling set
   before S10 applies when the DCVS starts). Behaviour without a caller:
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
6. The PMIC NTCs: B6, §12 (`piano-thermal-adc=1`; its own open questions in §12.9). The PMICs' temp-alarm stages (their
   temp-alarm@a00 peripherals) are still unread.
7. Does `reboot_kernel(RB_HALT)` power the tablet off natively (PSCI SYSTEM_OFF under Gunyah)? (one deliberate test, owner present.)
8. Is ±10 ms idle accounting enough for the governor, or does it need recount's mach-time idle (a k27 helper)? (the `gov` session's
   residency and the touch feel.)

## 11. Licence note

New code, BSD-2-Clause-Patent (`package/PianoThermal/LICENSE`). Linux (GPL) and the K90 port (APSL) were read for facts only; no code
was copied. The SCMI layouts are from Arm's public specification. B6 (§12): Linux's ADC5 gen3, VADC-common, SDAM, pinctrl-spmi-gpio
and PMIC-arbiter sources were read as the specification only; the thermistor curve is computed from the part's three published
parameters (§12.3), and Linux's resistance table is neither copied nor stored (`check-ntc-curve.py` reads it from a Linux tree at
run time to compare). §13: Linux's `tsens.c` / `tsens-v2.c` (the interrupt fields, the enable/disable sequences, the violation bits,
the trip clamp, the suspend rule) were read as the specification only; no code was copied.

## 12. B6: the board thermistors (PMIC ADC5 gen3)

Status: code, 2026-11-11. `Ntc.h`, `AdcChannels.h`, `PmicAdc.h`, `BoardTemps.h` (portable), `PianoThermalAdc.cpp` (the kernel
side, the kext's second object) and `package/include/PianoBoardTemps.h` (the API); host-tested (`adc_host: all passed`), the map
checked against the DT (`check-adc-map: 11 channels, 4 thermistor pins, the arbiter EE and the SDAMs match the DT`), linked into the
native KC (kernel 0059: `com.unixnu.driver.PianoThermal: linked, ... 435 pointers`, 17 native kexts, none from outside this repository); inert
without `piano-thermal-adc`; **not run on hardware**.

### 12.1 Purpose and gate

The battery's, the charger's and the two connector thermistors for charging safety (B4, PianoPower), the two skin thermistors (msm,
quiet) for the thermal policy, later the menu bar's battery temperature. Ubuntu exposed all eleven as monitoring-only thermal zones
(v0.7.0). A conversion is a write (to the ADC's SDAM, the PMIC's shared memory with its PBS sequencer), so it is opt-in:
`piano-thermal-adc=1`, default off; without it nothing of the SPMI arbiter is even mapped.

### 12.2 Hardware facts

Sources: DT = `out/inputs/piano-boot.dtb`; KD = the Ubuntu kernel's `sm8750-xiaomi-piano.dts` (piano/dev, pinned in `UPSTREAM`);
G3 = Linux `qcom-spmi-adc5-gen3.c` + `qcom-adc5-gen3-common.h`; VC = `qcom-vadc-common.c`; ARB = `spmi-pmic-arb.c`; all GPL, read as
the specification only.

| Item | Value | Source | State |
|---|---|---|---|
| Arbiter | SPMI PMIC arbiter v7, `qcom,ee = 0`; core 0x0c400000 (version, APID count, APID map +0x2000), cnfg 0x0c42d000 (owner table), EE 0's observer channels 0x0c440000 + 0x20·APID, read/write channels 0x0c500000 + 0x1000·APID (writes only for the APID's owner EE) | DT, ARB | verified: PianoPower and PianoInput read this way; k27 0057 and PianoDwc3 write SDAM 0x7100 this way on the tablet |
| ADC | PMK8550 (SID 0) `adc@9000` "qcom,spmi-adc5-gen3", reg <0x9000 0x9100>: two SDAMs; SDAM 0 = PPID 0x090, its channel 0 does the immediate conversions; interrupts on peripherals 0x90/0x91 (not used: polled) | DT, G3 | verified (DT, source); needs-boot |
| SDAM 0 bytes | 0x45 HS (0x01 ready, bit 7 busy), 0x46 STATUS1 (bit 7 fault, bit 0 EOC), 0x4a EOC_STS (bit 0 channel 0), 0x4b EOC_CLR, 0x4e CONV_ERR_CLR, 0x4f SID (bus [6:5], SID [4:0]), 0x50 PERPH_CH (bit 7 a conversion, else a status clear), 0x51 TIMER_SEL (1 immediate), 0x52 DIG_PARAM (calibration [5:4], decimation [3:2]), 0x53 FAST_AVG (bit 7 enable), 0x54 the ADC channel, 0x55 DELAY_CTL ([3:0] settle), 0x5c/0x5d the result, 0xe5 CONV_REQ (the SDAM's PBS trigger) | G3, `qcom-spmi-sdam.c` | verified (source) |
| Encodings | decimation {85, 340, 1360} → 0..2; settle {15, 100, 200, 300, 400, 500, 600, 700, 1000, 2000, 4000, 8000, 16000, 32000, 64000, 128000} µs → 0..15; one sample; calibration 1 = ratiometric | G3 | verified (source) |
| Codes | Gen3 ratiometric: code = 2^14·R/(R + 100 k), 0x8000 = no data; Gen4: code = 0xffff·R/(R + 100 k), 0x8000 valid (mid-scale) | G3, VC (Gen4: Ubuntu's own K:5812ec75bb03) | Gen3 source-verified; **Gen4 needs-boot** |
| Thermistors | 100 kΩ NTC to ground on a 100 kΩ pull-up inside the PMIC (the `*_100K_PU` channels), ratiometric, 1:1; Linux's curve is "NTCG104EF104" | DT, VC | verified (DT); the part from Linux |
| Pins | PM8550 GPIO 2, 3, 4 and PMIH0108 GPIO 6 high-impedance (the ADC node's pinctrl); PMIH0108 GPIO 12 carries charger1_therm but belongs to another EE, which keeps it high-Z (KD's comment) | DT, KD | verified (source); read at start |
| Zones | battery, charger1, conn, conn2, skin-msm, skin-quiet, xo, wifi, display, backlight, flash: `generic-adc-thermal`, no trips | DT | verified |

The channel map (`AdcChannels.h`, in the board line's order; `check-adc-map.py` compares every column but the role with the DT):

| Name | DT label | DT reg | PMIC (SID) | ADC channel | ADC | Decimation / settle | Zone | Role |
|---|---|---|---|---|---|---|---|---|
| batt | pmih010x_batt_therm | 0x744 | PMIH0108 (7) | 0x44 AMUX1_THM_100K_PU | Gen4 | 340 / 15 µs | battery | charging: battery |
| charger | pmih010x_charger1_therm | 0x74d | PMIH0108 (7) | 0x4d AMUX4_GPIO_100K_PU (GPIO 12) | Gen4 | 340 / 15 µs | charger1 | charging: charger |
| conn | pmih010x_conn_therm | 0x747 | PMIH0108 (7) | 0x47 AMUX4_THM_100K_PU | Gen4 | 340 / 15 µs | conn | charging: connector |
| conn2 | pm8550_conn_therm2 | 0x14b | PM8550 (1) | 0x4b AMUX2_GPIO_100K_PU | Gen3 | 1360 / 200 µs | conn2 | charging: connector 2 |
| msm | pm8550_cpu_therm | 0x144 | PM8550 (1) | 0x44 AMUX1_THM_100K_PU | Gen3 | 1360 / 200 µs | skin-msm | board (skin) |
| quiet | pmih010x_quiet_therm | 0x74a | PMIH0108 (7) | 0x4a AMUX1_GPIO_100K_PU | Gen4 | 340 / 15 µs | skin-quiet | board (skin) |
| xo | pmk8550_xo_therm | 0x044 | PMK8550 (0) | 0x44 AMUX1_THM_100K_PU | Gen3 | 1360 / 200 µs | xo | board |
| wifi | pm8550_wifi_therm | 0x146 | PM8550 (1) | 0x46 AMUX3_THM_100K_PU | Gen3 | 1360 / 200 µs | wifi | board |
| display | pm8550_display_therm | 0x149 | PM8550 (1) | 0x49 AMUX6_THM_100K_PU | Gen3 | 1360 / 200 µs | display | board |
| backlight | pm8550_backlight_therm | 0x14a | PM8550 (1) | 0x4a AMUX1_GPIO_100K_PU | Gen3 | 1360 / 200 µs | backlight | board |
| flash | pm8550_flash_therm | 0x145 | PM8550 (1) | 0x45 AMUX2_THM_100K_PU | Gen3 | 1360 / 200 µs | flash | board |

### 12.3 Scaling: from a code to degrees, derived (`Ntc.h`)

1. Resistance: R = 100 kΩ · code / (full scale − code), full scale 2^14 (Gen3) or 0xffff (Gen4), rounded to the ohm.
2. Temperature: Steinhart–Hart, 1/T = a + b·ln R + c·(ln R)^3, through three points fixed by the part's published parameters,
   R25 = 100 kΩ, B25/50 = 4250 K, B25/85 = 4311 K (a B value fixes R(T) = R25·exp(B·(1/T − 1/T25))): (25 °C, 100000 Ω),
   (50 °C, 33194.6 Ω), (85 °C, 8871.7 Ω). The solution a = 8.58857e-4, b = 2.05943e-4, c = 8.13564e-8 K⁻¹ is stored × 2^50
   (`adc_host --ntc` prints the derivation; the test solves it again in double precision and compares).
3. Integer only (the kext uses no floating point): ln R in Q32 (R = 2^k·m, ln m = 2·atanh((m − 1)/(m + 1)) by its series, within
   1e-8 of the library's), the polynomial in 128-bit products, one 64-bit division; within 0.001 °C of the formula in double from
   1 kΩ to 10 MΩ (tested); colder at higher resistance, negative above ~355.6 kΩ, 25.000 °C at 100 kΩ.
4. Plausible: −40..125 °C (the part's range; the extrapolation outside it is not trusted), else the channel is disabled.

How good: against the vendor's resistance–temperature data of the part (Linux's `adcmap7_100k`, 168 points, −40..127 °C;
`tooling/k27/check-ntc-curve.py <linux tree> out/native-kexts/PianoThermal/host-tests/adc_host` reads it at run time, nothing of it is
kept here) the curve is within **0.004 °C from 0 to 60 °C**, 0.12 °C below 0 °C and 0.46 °C above 60 °C, when the table's
temperatures are read as °C × 1024. Read as milli-degrees, as Linux returns them, the same data are 1.4 °C off in 0..60 °C and
3.5 °C at the top, and the part would miss its own R25 (100 kΩ sits at "25600"): **Linux, and so Ubuntu's v0.7.0 readings, report
these channels 2.4 % high** (25.6 °C at 25 °C, 51.2 °C at 50 °C). To compare with an Ubuntu reading, divide it by 1.024. The part's
tolerance (±1 % R25, ±1 % B) is worth about ±0.3 °C near room temperature; one Gen3 code is 0.005 °C, one Gen4 code 0.0013 °C.

### 12.4 The conversion (`PmicAdc.h`), the state machine the host test drives

Start (reads only): the arbiter must be v7; the APID of PPID 0x090 (an entry owned by EE 0 wins); its owner must be EE 0, else OFF
with nothing written. Only then is that one APID's read/write channel page mapped and writing enabled. Then (reads) SDAM 0's
TYPE/SUBTYPE, size register, HS, STATUS1, EOC_STS, its 7 command bytes and CONV_REQ, and the five thermistor pins' TYPE, MODE_CTL and
EN_CTL (high-Z = EN_CTL bit 7 clear).

One conversion of channel c:

| Step | Access | Bound / error |
|---|---|---|
| 1 ready | read HS until 0x01, then CONV_REQ until 0 | 20 ms (Linux 15 ms), else **handshake** |
| 2 stale status | read STATUS1..EOC_STS; a fault bit: status clear via CONV_ERR_CLR, wait ready, read again; an EOC bit: status clear via EOC_CLR, wait ready, read again | still set: **faultStuck** / **eocStuck** |
| 3 command | read 0x4f..0x55; write (one 7-byte write) SID = bus << 5 \| SID, PERPH_CH = 0x80, TIMER_SEL = 1, DIG_PARAM = (found & 0xc3) \| 1 << 4 \| decimation << 2, FAST_AVG = 0x80, the channel, DELAY_CTL = settle; read back | differs: **configMismatch** |
| 4 trigger | write CONV_REQ = 1 | |
| 5 end | read STATUS1..EOC_STS: a fault → status clear via CONV_ERR_CLR, **convFault**; EOC_STS bit 0 → done | 600 ms (Linux 501 ms), else **convTimeout** |
| 6 result | read 0x5c/0x5d (little-endian) | |
| 7 status clear | write EOC_CLR = 1, PERPH_CH = 0, CONV_REQ = 1 (Linux's) | the next step 1 waits for it |
| 8 scale | §12.3; Gen3 0x8000 **invalidData**; 0 or a negative Gen3 code **shorted**; at or above full scale **open**; outside −40..125 °C **implausible** | |

Waits spin at 100 µs for their first millisecond, then sleep 1 ms per poll; every arbiter transaction waits at most 1000 × 1 µs
(Linux's) and ends done, failure/denied/dropped, or **spmiTimeout**. Step 2 and step 3's read-back are not in Linux: a poll without
the interrupt would take a stale EOC for its own, and the read-back proves the SDAM holds the command before the PBS is triggered.

Writes, all of them: bytes 0x4b, 0x4e, 0x4f..0x55 and 0xe5 of SDAM 0x9000, through its own APID's read/write channel. `writable()`
is that allowlist, and `Bus::write` refuses anything else before a store (`refused`); the arbiter's core and owner table are never
written; everything else (the survey, the pins, every other PMIC peripheral) is an observer read. Per conversion about 21 reads and
5 writes (the model: 20.8 and 55 a round). The host model audits every write and fails on one outside the allowlist, on another
APID, on core/cnfg, or on a trigger written while one is pending.

### 12.5 Errors

| Error | Scope | After it |
|---|---|---|
| notArbiterV7, noApid, notOwned, spmiFailed / spmiDenied / spmiTimeout, handshake, eocStuck, faultStuck, configMismatch, refused | the shared machinery | the ADC **stops** for the boot: every channel off, `BoardAdcState` `stopped: <why>`, the API STOPPED |
| convTimeout, convFault, invalidData, open, shorted, implausible | one channel | that channel is **disabled** for the boot (`BoardAdc` `off: <why>`), the others go on |

A PBS that drops a conversion costs that channel (600 ms, once); a PBS that wedges costs the channel, then the next channel's
handshake stops everything (at most ~620 ms). Nothing is retried: a disabled channel is unknown, and the charging rules treat it so.

### 12.6 The kernel side (`PianoThermalAdc.cpp`)

A thread of its own (`piano-thermal-adc`), so a slow conversion never delays the TSENS polling (10 ms near a trip). It starts after
`piano-thermal-delay`, surveys, converts at once, then every `piano-thermal-adc-s` (default `piano-thermal-log-s`, 30 s; 5..3600).
Each round: the board line, the API snapshot, the properties; the first round also the raw codes (every round with
`piano-thermal-adc=2`) and a statistics line (again every 10th round and with any change). Boot arguments: §6; properties: §5.
The hook in `PianoThermal.cpp` is four small hunks: the include, `piano-thermal-adc` implying `piano-thermal=1`, `start()` calling
`adc::hookStart`, and the `"piano-thermal-board-temps"` platform function. Cost: 11 conversions every 30 s, mostly asleep in 1 ms
polls (the first round's statistics line gives the real duration).

### 12.7 Charging safety: the API for PianoPower

`package/include/PianoBoardTemps.h` (C and C++):

```c
struct piano_board_temps { uint32_t version, state, valid, disabled; int32_t milli_c[4]; uint32_t age_ms[4]; uint32_t period_ms, rounds; };
int piano_thermal_board_temps(struct piano_board_temps *t);   /* t->version = PIANO_BOARD_TEMPS_VERSION; the valid count, -1 bad argument */
/* or, without a link dependency, on IOService::serviceMatching("PianoThermal"): */
svc->callPlatformFunction("piano-thermal-board-temps", false, &t, nullptr, nullptr, nullptr);   /* kIOReturnSuccess / BadArgument */
```

Indices: 0 battery (batt), 1 charger, 2 connector (conn), 3 connector 2 (conn2). `state`: OFF (no `piano-thermal-adc`), STARTING,
RUNNING, STOPPED. A temperature counts only with its bit in `valid` (a working channel, at most three periods old); `disabled`
names the channels lost to an error. Lock-free (a sequence count around a copy of the last round), any thread, never blocks, no
SPMI access. Linking needs PianoThermal named as a library in PianoPower's descriptor and listed before it (today it is listed
after); the platform function needs neither.

How B4 takes it (`docs/drivers/charging-policy.md`, `package/PianoPower/ChargePolicy.h`; a proposal for its owner, the numbers
below not stock-verified):
- these readings may only make charging more conservative than the policy without them and the DSP's own JEITA window, never
  less; missing = unknown, never cool (the policy's "none" branch: 500 mA);
- battery: the batt NTC joins the derating's t, "the warmest of the firmware's 0x202a and the two packs", as a third source, and
  the coolest-pack checks as a third reading (in `PianoChargeInput`: `t_ntc_dc` and a `PIANO_CHG_IN_NTC` valid bit, filled by
  PianoPower from `piano_thermal_board_temps()` each cycle); the NTC and the packs more than 10 °C apart is logged, and the warmer
  counts;
- connectors: the policy keeps the input limit off the battery temperature on purpose (a warm battery is not left to carry the
  system); a hot connector is the input current's own heating (contact resistance, a damaged cable, lint), so here the input limit
  is what to lower: conn or conn2 at or above 60 °C → ICL 100 mA (as at detach) until both are under 50 °C, logged;
- charger at or above 85 °C → ICL 500 mA until it is under 75 °C.

### 12.8 Hardware test (one RAM boot; unattended is fine, nothing is driven)

```sh
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=300 \
  PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-thermal=1 piano-thermal-adc=1 piano-power=1" tooling/k27/build-native-image.sh
tooling/native/native-loop.sh out/ram/boot-piano-macos-ram-A-bundled.img
```

Expected (numbers are examples; the codes match the temperatures shown):
```
PianoThermal: adc: piano-thermal-adc=1: the PMIC ADC5 gen3 (PMK8550 SDAM 0x9000, PBS) for the 11 board thermistors, a round every 30 s; writes: ...
PianoThermal: adc: spmi arbiter version 0x7..., N APIDs; SDAM 0x9000 (PPID 0x90): APID n (1 map entries), owner EE 0 (ours)
PianoThermal: adc: SDAM 0x9000: type 0x.. subtype 0x.., size register 0x..; HS 0x1, STATUS1 0x0, EOC_STS 0x0, CONV_REQ 0x0; command bytes ... (idle)
PianoThermal: adc: thermistor pins: pm8550 gpio2 high-Z (type 0x10, mode 0x.., en 0); ...; pmih0108 gpio12 (firmware's) high-Z (...);
PianoThermal: adc: channels (DT reg, ADC): batt 0x744 g4, charger 0x74d g4, conn 0x747 g4, conn2 0x14b g3, msm 0x144 g3, ...
PianoThermal: adc: running: a round every 30 s
PianoThermal: [0s] board: batt 31.2 C, charger 33.0 C, conn 30.1 C, conn2 30.4 C, msm 35.2 C, quiet 31.0 C, xo 36.0 C, wifi 34.1 C, display 33.3 C, backlight 33.8 C, flash 32.9 C
PianoThermal: [0s] adc raw (code, kOhm): batt 0x6db6 74.9 charger 0x689f 69.1 conn 0x70e1 78.8 conn2 0x1c01 77.7 msm 0x18a5 62.6 ...
PianoThermal: [0s] adc: round 1: 11 conversions, 0 failed, N us each on average, N us at most; 11 of 11 channels on; 0 stale status clears; N SPMI reads, 55 writes
PianoThermal: [30s] board: ...
PianoPower: [30s] battery: ... 30.9 C ...
```

Pass: owner EE 0 and an idle SDAM; eleven readings between 15 and 50 °C at idle that move slowly; at 20..35 °C raw codes of about
0x6300..0x9000 (Gen4) and 0x1800..0x2400 (Gen3), the Gen4 readings agreeing with their Gen3 neighbours (that settles the Gen4 full
scale); batt within 3 °C of PianoPower's gauge temperature; msm and quiet rising under load; conversions of milliseconds in the
statistics line; no DISABLED or STOPPED line. Then a session with `piano-thermal-adc-s=5` to watch the battery while charging.

Failure meanings: `OFF: SDAM 0x9000's APID belongs to another EE` (not this processor's to write: nothing written, B6 needs another
route); `(BUSY or not an ADC SDAM ...)` and then `STOPPED ... handshake` (another master uses SDAM 0, or the PBS is wedged since the
boot stages); `STOPPED ... command bytes read back differ` (not memory-like: a wrong APID or a protected SDAM); `DISABLED: no end of
conversion` on every channel (the PBS does not post EOC_STS without its interrupt enabled: poll STATUS1's EOC, or take the SDAM
interrupt); Gen4 channels far from their Gen3 neighbours while those are sane (the Gen4 full scale is not 0xffff); one channel
`open` / `shorted` (its thermistor or pin: see its pin's `ENABLED`); all readings about 2.4 % below Ubuntu's (expected, §12.3).

### 12.9 Risks and open questions

1. Gen4's full scale (0xffff) and 0x8000-is-valid come only from Ubuntu's own Gen4 patch; the first raw codes settle it.
2. Polling instead of the interrupt: the PBS is expected to set EOC_STS whatever the interrupt enables (Linux's handler reads the
   same bit); unproven. Fallbacks: poll STATUS1 bit 0 as well (one line in `convert`), or the SDAM interrupt once the PDC/GIC
   helper lands. The SDAM may raise its interrupt at every EOC: nothing enables that APID's interrupt in the arbiter natively, and
   XNU enables no SPI it has no handler for, so nothing fires.
3. Another master on SDAM 0 (a boot stage, a DSP firmware): the handshake and the stale-status checks catch it, and the result is
   a stop, never a fight. Ubuntu used SDAM 0 from EE 0 the same way.
4. Observer channels are per APID and not locked across kexts: PianoPower's start reads TYPE/SUBTYPE of every SID 7 peripheral,
   PMIH0108 GPIO 6 and 12 included, which this survey reads once; a simultaneous read could garble one survey value (no write is
   involved). No other kext reads the SDAM's APID.
5. Suspend: a round may be in flight when s2idle begins (the PMIC stays on: harmless); the suspend contract should pause the ADC
   thread (a flag checked before each conversion) once docs/power.md's hooks land.
6. The pins are only read: a pin found driven makes its channel suspect and is reported, not fixed (GPIO writes are outside B6's
   write rule; GPIO 12 is another EE's anyway).
7. The PMICs' temp-alarm stages and the ADC's own threshold monitoring (ADC_TM, SDAM 1) stay unused; the latter would let the PBS
   watch a threshold without polling (a later step, after one good run).

## 13. The uplow interrupt: `piano-thermal-irq=1`

Status: code, 2026-11-11. `TsensIrq.h` (portable: the facts, the threshold encoders, the handler's decision, the state machine),
`PianoThermalIrq.cpp` (the kernel side, the kext's third object) and a hook of a few lines in `PianoThermal.cpp`; host-tested
(`uplow_host: all passed`, every scene under three models of the controller's status latch), the interrupt table checked against
the DT at every build (`check-tsens-irq: 4 controllers: uplow/critical SPIs, level high, GIC parent match the DT`), linked into the
native KC (kernel 0059: `com.unixnu.driver.PianoThermal: linked, ... 490 pointers`; `native kext rule: 21 native kexts linked, none
from outside this repository`); default off; **not run on the tablet**: native SPI delivery under Gunyah is unproven, and the
batched test (§13.8) is built to settle it at the first boot.

### 13.1 Purpose

Battery. The thread polls every TSENS sensor every 1 s at rest, every 250 ms once the hottest reaches 70 C, and every 10 ms near a
trip or while a cluster runs above 2.5 GHz (§5); the last alone is 100 wake-ups/s for as long as the boot stages leave the prime
cores fast (`piano-cpufreq=1` reports, sets nothing). Those tiers exist because a 1 s poll can step over a whole heating ramp
(~200 C/s). The upper-threshold interrupt says at once when any sensor crosses 85 C, so while every sensor is under it the warm tier
and the fast-clock rule are not needed and the base poll can be 5 s; the lower-threshold interrupt says when the hot phase is over.

### 13.2 Facts

Sources: DT = `out/inputs/piano-boot.dtb`; TSV2 = Linux `drivers/thermal/qcom/tsens.c` + `tsens-v2.c` (Ubuntu's tree, GPL, the
specification only); PI = `docs/power-irq.md` §2.1.

| Item | Value | Source | State |
|---|---|---|---|
| Lines | "uplow" tsens0..3 = SPI 771 / 772 / 773 / 774 (`<0 0x303..0x306 4>`), "critical" SPI 484-487 (`<0 0x1e4..0x1e7 4>`), level high; the nodes inherit the root's `interrupt-parent`, the GICv3 | DT | verified (`check-tsens-irq.py`) |
| PDC | SPI 771-774 are the outputs of PDC pins 26-29 (`qcom,pdc-ranges` first range <0 745 51>); with the GIC as the nodes' parent the PDC is never programmed for them (Linux's PDC domain only sees interrupts parented to it), so the firmware's PDC setting passes them | DT | inferred; nothing here touches the PDC |
| Interrupt registers | TM_INT_EN 0x04 [2:0]: v2's separate critical, upper and lower enables (which bit is which is not stated; Linux writes 7); TM_UPPER_LOWER_INT_STATUS 0x08, _CLEAR 0x0c, _MASK 0x10: sensor n's lower interrupt at bit n, its upper at bit 16 + n | TSV2 | spec |
| Thresholds | TM_Sn_UPPER_LOWER_THRESHOLD 0x20 + 4n: lower [11:0], upper [23:12], signed deci-C; v2 clamps trips to -40..120 C | TSV2 | spec; the readout's decode is verified on SM8750 (§2) |
| Semantics | a threshold is violated while TM_Sn_STATUS bit 17 (LOWER) / 18 (UPPER) is set (Linux reads those, not 0x08); enable = mask bit 0; disable = mask bit 1, then the clear bit 1 and 0; set_trips writes the window, then unmasks; before system suspend every lower interrupt masked, the upper ones armed (08e750858038) | TSV2 | spec |
| Delivery | ArmCpu's `armcpu_spi_attach` (INTID = SPI + 32: 803-806), level, routed to the boot CPU, priority 0xa0, one handler per INTID, no detach and no mask (a storm is stopped with GICD_ICENABLER) | PI | the attach verified (kernel symbols); delivery **needs-boot** |

### 13.3 Design

- **The window.** Every sensor that has a reading at the first poll gets lower 70 C / upper 85 C (one TM_Sn_UPPER_LOWER_THRESHOLD
  write each, bits [31:24] as found). An armed sensor is *low* (upper unmasked, lower masked) or *high* (the reverse): an upper
  interrupt makes it high, a lower one low again; after the arming only the masks change. A sensor without a reading at the start
  stays masked and is polled as today.
- **The proof**, per controller, before anything relies on the line: every up/low interrupt masked and cleared, the SPI attached,
  TM_INT_EN set to 7 when found otherwise, then the controller's hottest sensor gets an upper threshold 5 C under its reading and
  must interrupt within 250 ms, then a lower threshold 5 C above it, the same; then its window. Any failure: off for the boot. The
  proof costs two interrupts per controller at the start and shows, in the log, that the hardware compares, masks and clears as
  read, and that the hypervisor delivers each line.
- **The handler** (primary interrupt context, the boot CPU, under a spin lock the thread takes with interrupts off): every
  interrupt that is up and unmasked (its TM_Sn_STATUS bit, or latched in 0x08) is masked and its clear pulsed, which drops the
  level line; a latched status of an interrupt already masked is cleared; the bits are recorded and the thread woken. Nothing of
  ours up: a stray for PianoIrq.h's storm guard (more than 2000 in 100 ms disable the line at the GIC).
- **The thread**, each poll: takes the records; an upper interrupt on a low sensor makes it high and holds the fast tier (10 ms)
  for 1 s, after which today's tiers apply until it is low again; a lower interrupt on a high sensor makes it low. An interrupt
  against the reading (upper with the sensor under 70 C, lower with it over 85 C) is spurious and re-armed. Masks are written
  against the register (the handler changes it on its own); a bit about to be unmasked has its clear pulsed first (a stale latch);
  a bit recorded but not yet processed stays masked.
- **Rest** = every armed sensor low, no unarmed sensor at 70 C or more, no fast hold, no test, nothing suspect. Safety: under 85 C
  nothing in the policy acts (warm is 85, passive 105/118, critical 115 C); 85 C is 20 C under the lowest passive trip, and at
  200 C/s the fast hold reaches the near-trip tier (95 C) within ~50 ms of the interrupt.
- **Misses**: an armed sensor 2 C past its threshold with no interrupt is suspect: the fast tier at once (when it is a rise), 50 ms
  of grace, then a miss, and the poll changes its state itself. Three misses in a row (an interrupt in between resets the run).
- **The fallback**, logged once: three misses in a row, more than 40 interrupts in 10 s ("stormy"), a line's storm guard, a proof
  or an attach that fails. Every up/low interrupt masked, the thresholds and TM_INT_EN written back as found, the lines disabled at
  the GIC (ArmCpu has no detach); today's polling for the rest of the boot; the property `UplowIrq` says `off: <why>`.
- **Never written**: the critical thresholds (0x60 + 4n), the critical interrupt registers (0x14-0x1c), SROT, the PDC. The hardware's
  own protection stays as the boot stages left it.

### 13.4 The tiers

| Uplow state | Poll tier | Base |
|---|---|---|
| off (default), starting, fallen back | today's (§5) | today's (1 s, or the governor's 100 ms tick) |
| at rest | fast only for a thermally limited cluster; the warm tier and the fast-clock rule stand down | 5 s (`-poll-ms`; 0: the report period), never the governor's tick |
| an upper interrupt (or a suspect rise) | fast (10 ms) for `-hold-ms` (1 s) | today's |
| a sensor high, a test running, a sensor suspect, an unarmed sensor at 70 C or more | today's | today's |

The thread's wait at rest is the uplow path's (`PianoThermal::pollWait` hands over to it): an interrupt ends it at once, and so does
the system-sleep park request (it also kicks that wait). The 30 s report and the policy run as before, on the iterations there are.

### 13.5 Boot arguments

`piano-thermal-irq=1` (needs PianoThermal running: `piano-thermal` ≥ 1, or `piano-cpufreq` / `piano-thermal-adc`, which imply it),
`-up` / `-down` (85 / 70 C), `-poll-ms` (5000), `-hold-ms` (1000), `-test` / `-test-s` (0 / 120): §6.

### 13.6 System sleep and the suspend hooks

With `piano-thermal-irq=1` the power-tree sleep (§5) masks as with `piano-thermal-sleep-mask=1` (the contract's rule: masked when
interrupts are used). While the path is live, `piano_thermal_suspend_prepare()` is the path's own: every lower interrupt masked and
its clear pulsed, the upper ones left as the states have them (armed for every low sensor: overheating still interrupts), no lower
interrupt unmasked until finish; a high sensor that cools meanwhile goes low by the poll, without a miss; `finish` writes the path's
masks again. Not live (off, starting, fallen back): the hooks' own path as before (lower and upper masked, the masks found put
back). A suspend that began while the path was starting keeps every lower interrupt masked through an arming that happens meanwhile,
and its finish writes the path's masks instead of the older ones found. Lines: `system sleep: in the power tree (0): the thread
parks while suspended, the threshold interrupts masked (piano-thermal-irq=1: the lower ones while it is live)`, then per sleep
`suspend prepare: lower threshold interrupts masked on 4 controllers, the upper ones kept armed (the uplow interrupt is serviced):
tsens0 0x7fff->0x7fff ...` and `suspend finish: the uplow interrupt masks are back (lower armed for the sensors above 85.0 C)`.

### 13.7 Build and host tests

`tooling/k27/build-native-piano-thermal.sh` runs `package/common/tests/run.sh` (PianoIrq.h), then `tests/uplow_host.cpp` against a
model of the four controllers (thresholds compared with each sensor's temperature, the status bits, mask, clear, TM_INT_EN, a status
latch that is absent, of the unmasked interrupts, or of every violated threshold) and of a level line that calls the handler while
it is up, attached, enabled at the GIC and delivered, and never while the thread holds the lock:

- the encoders (window word, clamp, the bits), the handler (masks exactly what is up and unmasked, pulses its clear; a stray; a
  latched masked status cleared);
- the start: the quiet masks, TM_INT_EN 0 → 7 (found 7: untouched), the proof on each controller, the windows (bits above kept,
  an unusable sensor untouched), a hot sensor armed high, the masks; a second start refused;
- the tiers on interrupts: rest (warm → base, the fast-clock rule gone, a limited cluster still fast, base 5000), nothing inside
  the hysteresis, the upper interrupt (high, the fast hold, base back to 1000), today's tiers after the hold, the lower interrupt
  (rest again); an unarmed sensor at 72 C keeps the warm tier; the governor's tick never relaxed;
- misses: 1 C past is not suspect, 3 C past is (fast at once), the miss after the grace, a delivered interrupt resetting the run,
  three in a row → `OFF (3 missed interrupts in a row)` once, all masked, the windows and TM_INT_EN back, lines disabled, today's
  tiers, nothing more logged; the hooks then -1;
- proofs that fail (a line never delivered; an attach refused; nothing mapped: no write at all);
- storms: a line stuck up (the guard disables it; at the attach too), a status stuck at "upper" (spurious, re-armed, until
  `OFF (stormy ...)`);
- suspend: lower masked with upper armed, a cool-down while suspended (no interrupt, no miss), an overheat while suspended (the
  upper interrupt, lower kept masked), finish, a fallback while suspended, the caller's own suspend around a start;
- the synthetic test: PASS, a missed one, no crossing in time, the summary, the windows back, rest afterwards;
- a record that lands between the step's take and its mask write stays masked and is processed next step; and in every scene
  the model refuses any write outside TM_INT_EN, the up/low clear and mask, and the thresholds 0x20 + 4n.

With the DT, `tooling/k27/check-tsens-irq.py` compares the SPI table (`uplow_host --map`) with every TSENS node (names, cells,
trigger, inherited parent) and prints which PDC pin feeds each line. The kext's objects link with 0 unresolved imports
(`armcpu_spi_attach`, `armcpu_spi_count`, the spin lock and wait primitives from the kernel).

### 13.8 The batched tablet test (synthetic, nothing heated)

Heating the SoC to 85 C is not needed and not wanted. The proof at the start already makes each line fire twice by moving a
threshold across the reading; the synthetic test then waits for the *temperature itself* to cross a threshold set just above it:
`piano-thermal-irq-test=N` puts, per controller, the hottest low sensor's upper threshold N tenths of a degree above its reading
for `-test-s` seconds and then restores 85 C. The test starts at PianoThermal's first poll, early in the boot, and the rest of the
boot (WindowServer, the GPU from `piano-gpu-delay`) is expected to warm the hottest sensors by a degree or more, so N = 10 (1.0 C)
should cross without any load (if not, a few seconds of use at the login window will); nothing ever gets hot.

Images (RAM boot only; the GPU profile's seed and trust cache as in §8.3; images are never committed):

```sh
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=420 PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2" tooling/k27/build-native-image.sh
for m in "uplow-test:piano-thermal=2 piano-thermal-irq=1 piano-thermal-irq-test=10 piano-thermal-irq-test-s=240" \
         "uplow-report:piano-thermal=2 piano-cpufreq=1 piano-thermal-irq=1" \
         "uplow-report-A:piano-thermal=2 piano-cpufreq=1"; do
  PIANO_RESET_AFTER=420 PIANO_EXTRA_ARGS="piano-gpu=2 piano-gpu-delay=20 panic_on_crash_disabled=1 piano-usb=2 piano-power=1 ${m#*:}" \
    python3 tooling/bootimg/build-ram-images.py --fd out/fd/piano-uefi-A-bundled.fd --fd-name A-bundled --kc out/k27/kc-k27-native.bin \
    --trust out/turnip-a830/stage/kernel.macho.trustcache --seed out/turnip-a830/stage/scratch-data.img --adt out/adt/piano-k27.adt \
    --utc "$(date +%s)" --out "out/ram-${m%%:*}"
done
tooling/native/native-loop.sh out/ram-uplow-test/boot-piano-macos-ram-A-bundled.img out/ram-dump/boot-piano-macos-ram-A-bundled.img
```

Then `ram-uplow-report` and its A side `ram-uplow-report-A` (today's polling) the same way, on battery, screen on at the login
window, no input. From `logs/native-runs/*-loop/xnu-console.txt`: `grep -E 'PianoThermal: .*(uplow|idle:|polling)|battery:'`.

Built 2026-11-12 from this change on main fc86525 (KC sha256 `1fe9043ee41df7fc`, kernel `kernel-k27-0059-native` `d475f824a4f234af`;
the GPU profile's seed and trust cache; `PIANO_VMM_PRESENT=0`; all with `panic_on_crash_disabled=1 piano-usb=2 piano-power=1
piano-gpu=2 piano-gpu-delay=20`, reset after 420 s); the macOS RAM image of each is `boot-piano-macos-ram-A-bundled.img`:

| Dir (`out/`) | Arguments added | sha256 (16) |
|---|---|---|
| `ram-uplow-test` | `piano-thermal=2 piano-thermal-irq=1 piano-thermal-irq-test=10 piano-thermal-irq-test-s=240` | `8b91737ed685e152` |
| `ram-uplow-report` | `piano-thermal=2 piano-cpufreq=1 piano-thermal-irq=1` | `2bdbe89fd90f9db1` |
| `ram-uplow-report-A` | `piano-thermal=2 piano-cpufreq=1` (today's polling) | `c706829ed03d29d6` |
| `ram-dump` | the dump image (reset after 1 s, `data-persist=0`) for all three | `717043802bb7d69a` |

Expected in `ram-uplow-test` (numbers are examples):

```
PianoThermal: system sleep: in the power tree (0): the thread parks while suspended, the threshold interrupts masked (piano-thermal-irq=1: the lower ones while it is live)
PianoThermal: uplow: piano-thermal-irq=1: the TSENS upper/lower threshold interrupts (SPI 771-774, level), upper 85 C / lower 70 C on every sensor; at rest the base poll is 5000 ms (piano-thermal-irq-poll-ms), the warm tier and the fast-clock rule stand down; an upper interrupt polls every 10 ms for 1000 ms; a proof on each controller first, then the synthetic test (piano-thermal-irq-test)
PianoThermal: uplow: tsens0 cpu0: SPI 771 (INTID 803, level) attached (0): readback 0xa003 (group 1 1, enabled 1, edge 0, priority 0xa0), router 0x...
PianoThermal: uplow: tsens0 cpu0: proof on s7 cpu-0-3-0 at 41.2 C: upper 36.2 C -> interrupt in 0 ms, lower 46.2 C -> interrupt in 0 ms (found: mask 0x..., int-en 0x...)
   ... the same for tsens1 (SPI 772), tsens2 (SPI 773), tsens3 (SPI 774)
PianoThermal: uplow: live: 47 sensors armed on 4 controllers (upper 85.0 C, lower 70.0 C; 0 above the upper threshold); masks tsens0 0x7fff tsens1 0x7f tsens2 0xffff tsens3 0x1ff; at rest the base poll is 5000 ms, an upper interrupt polls fast for 1000 ms
PianoThermal: uplow: test: tsens0 s7 cpu-0-3-0 at 41.2 C: upper threshold 42.2 C (piano-thermal-irq-test=10) for up to 240 s; warm the SoC a little (a short CPU load) to cross it
   ... one per controller
PianoThermal: [23s] uplow: test: tsens0 s7 cpu-0-3-0: upper interrupt at 42.3 C after 17.6 s (threshold 42.2 C, from 41.2 C; SPI 771 count 3): PASS; threshold back to 85.0 C
   ... one per controller (PASS, or "no crossing in 240 s (highest ...)")
PianoThermal: [61s] uplow: test: done: 4 passed, 0 missed, 0 without a crossing
PianoThermal: [61s] uplow: at rest again (every sensor under 85.0 C, or back under 70.0 C after a crossing): the base poll is 5000 ms
PianoThermal: idle: 13 wakeups in 60 s, irq 0 (timer 13, kick 0; uplow live at rest, at rest 59 s; SPI 771-774 counts 3 3 3 3; 4 interrupts, 0 misses)
```

Pass: four `attached (0)` lines with readback group 1, enabled 1, edge 0, priority 0xa0; four proof lines with interrupts in a
few ms; `live: 47 sensors armed on 4 controllers`; at least one test PASS (a "no crossing" is the temperature's, not the path's:
repeat with `piano-thermal-irq-test=5`); after the test `idle:` lines of about 12-13 wake-ups a minute with `at rest` near 60 s
and 0 misses; no `uplow: OFF`; the 30 s reports as before.

In `ram-uplow-report` (the boot stages' clocks kept, `piano-cpufreq=1`): if `cpufreq d1 ...: now 4320 MHz` (above 2.5 GHz), the A
side `ram-uplow-report-A` logs `polling every 10 ms: a cluster runs above the fast-poll clock` and `[30s] polling: ... fast 30000
ms`, the uplow side none of that (`idle:` ~12 a minute at rest); the battery current of the two (PianoPower's 30 s lines) is the
measure of what the interrupt saves.

Failure meanings:

- `uplow: OFF (proof: tsensN sS ...: upper threshold ... and no interrupt in 250 ms (status 0x..., int status 0x..., mask 0x...,
  SPI 77x count 0)) at the start`: the line was not delivered. Status bit 18 (0x40000) set means the hardware saw the crossing (the
  write took, the comparison works) and the interrupt did not arrive: Gunyah does not forward SPI 771-774, the PDC pin is not
  passing it, or TM_INT_EN 7 does not enable it; bit 18 clear means the threshold write did not take (compare the `found` values).
  The int status shows whether 0x08 latches (bit 16 + S). Polled from then on: nothing else changes.
- `NOT attached (0x...)`: no ArmCpu slot or the INTID taken (another kext); polled.
- `uplow: OFF (storm: SPI 77x stayed up ...)`: masking does not drop the line (its level comes from elsewhere); the GIC line is off
  and the sensors are polled.
- `uplow: OFF (stormy: ...)` after a run of `spurious (re-armed)` lines: a status that reads violated against its reading (a stuck
  comparator, or the window not what was written).
- `MISSED` lines, then `OFF (3 missed interrupts in a row)`: the line stopped arriving after the proof.
- A hang or reset right after an `attached` line: a storm the guard did not stop; drop `piano-thermal-irq`.
- A data abort at the first proof write: the TM range is read-only to this EL1 (Linux writes these registers under the same
  hypervisor, so not expected); drop `piano-thermal-irq`.

### 13.9 Risks and open questions

1. Delivery under Gunyah (SPI 771-774 are PDC outputs; the HLOS DT names them; Linux takes them): unproven natively. The proof
   decides at each boot; without delivery the cost is one 250 ms wait at the start (the proof stops at the first failure), then
   today's polling.
2. TM_INT_EN's bit order is not in the specification read; the path writes Linux's value (7) only when the boot stages left it
   otherwise, which also sets the critical interrupt's enable: its SPI (484-487) has no handler and stays disabled at the GIC, and
   the critical *thresholds* and mask are not touched. Restored as found on a fallback.
3. TM_UPPER_LOWER_INT_STATUS's latch semantics are not documented; the handler masks on the TM_Sn_STATUS violation bits as Linux
   does, and the host test runs every scene with no latch, a latch of unmasked interrupts and a raw latch.
4. The relaxed base trusts the upper interrupt for every rise under 85 C: a sensor without a reading at the start is not armed and
   keeps today's polling from 70 C; a sensor whose reading stops (VALID lost) keeps its armed interrupt.
5. The handler runs on the boot CPU with a spin lock the thread holds for a few register accesses with interrupts off; a storm on
   CPU 0 is bounded by the guard (2000 entries).
6. Suspend: the upper interrupts stay armed across a sleep while the path is live (Linux's rule); whether one wakes the SoC from a
   deep state depends on the PDC (not programmed here) and the sleep driver.
7. Not done: per-crossing windows (Linux's set_trips moves the window to the next trip; here the window is fixed and only the masks
   change), the critical interrupt, and an interrupt-driven near-trip tier (the 10 ms polls above 95 C stay).
