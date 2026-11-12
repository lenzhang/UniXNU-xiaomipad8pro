# Charging and battery: PianoPower (fuel gauges, the charger PMIC's port, the DSP's charger firmware)

Status 2026-11-11: `package/PianoPower` built and linked into a test KC (host tests pass); **not run on the tablet**. Inert unless
`piano-power=N`. Gate for making macOS the default OS (the local plan `persistent-boot-plan.md` §3.2 and §4, not published).

## 0. The answer

- **Charging control on SM8750/piano is the audio DSP's (ADSP) job in every stock and Linux stack.** The buck charger is a block
  of the PMIH010x PMIC (SID 7) driven by the DSP's charger firmware. Mu-Silicium's piano DXE list drops the stock PmicDxe because
  it "faults in SPMI on channel 690, which ABL no longer gives the apps EE" (`Mu-Silicium-piano/Platforms/Xiaomi/pianoPkg/Include/DXE.inc:61`).
  **Correction from the level 1 measurement (end of this page):** the charger, DCDC, USBIN and Type-C blocks have write owner
  **EE 0, which is this processor** (the arbiter node's `qcom,ee = <0>`, the EE PianoInput and PianoDwc3 read as), not the DSP.
  So design (a), writing the charger over the arbiter's write channel, is not ruled out by ownership; what it lacks is the
  PMIH010x charger register map (no BSD or mainline source: Linux on piano left the charger to the DSP). Until that map exists,
  (b) stays the path; with the DSP running, its firmware and such writes would also fight.
- **The DSP is not running when our UEFI hands over.** Linux started it itself (remoteproc PAS, `auto_boot`). The stock XBL
  charger driver loads the DSP's charger firmware only to charge a dead battery and then reboots ("Chg Fw Load not required boot
  to HLOS", "Device Will Reboot After ADSP Charging", strings of `piano-binaries-own303/.../QcomChargerDxeLA.efi`). Our FD
  carries a patched QcomChargerDxeLA that does nothing at run time (`IsChargingSupported Cannot locate PmicSchgProtocol: Not
  Found`, the local bring-up logs (`uefi-A-debug-222920/pianolog.txt:2001`, not published); its config writes and watchdog enable are
  stubbed, `piano-uefi-UniXNU-piano/tools/patch-qcom-dxe.sh`), loads no DSP and pets no charger watchdog. Gunyah does not own
  the DSP: its node and carveouts are ordinary HLOS ones (`docs/hyp-evidence/`), and PAS is a plain SCM call from the HLOS.
- **Without the DSP the PMIC keeps charging on the configuration the boot stages left**, with no software control: measured on
  the SM8750 OnePlus 13T (+311 mA at 100 %, `unixnu-research/UniXNU/docs/devices/oneplus13t.md:1273`). On piano that state is
  **unmeasured**; "native XNU drains even on the cable" (plan §4.4) was extrapolated from Ubuntu's 11:11 incident, where the DSP
  *was* running at its 100 mA power-on input limit. PianoPower level 1 measures exactly this (gauge current on the cable).
- **What Ubuntu did.** `piano-charging.service` writes nothing to sysfs: it loads `pmic_glink_altmode` and sets `xiaomi_mca`'s
  module parameters `hv_charge`, `charge_current`, `full_charge`, `mipps_auth`, `cp_charge`, `pmic_glink_altmode.pan_ack` and last
  `charge_policy` to 1; stop sets `charge_policy=0`. The driver then talks to the DSP: input limit by charger type (CDP 900 mA,
  DCP/QC/PD at 5 V 1500 mA, 9 V PD 1600 mA, 9 V PPS/verified 2500 mA, 100 mA on detach), charge current by type with thermal
  derating (500 mA below 15 °C, -300 mA/°C above 38 °C), 9 V from a fixed PDO after a 15 s wait, the SC8541 pump from PPS
  sources, and acknowledges the DSP's USB-C port notifications. The DSP itself keeps a 4400 mV float voltage (0x2007 reads
  2·mV − 3600, so 5200; stock full is 4480 mV, 4060 mV from 48 °C) and a 0–55 °C JEITA window (`docs/ubuntu-history/device.md`
  §2.5). Measured: 8.8 V / 1.59 A in, +1.2–1.34 A per cell at light load (`streamC-charging-report.md`).
- **What the VM path did.** macOS was a crosvm guest; the Ubuntu host kernel (pmic_glink + xiaomi_mca + piano-charging) charged
  the tablet and the guest saw no battery (`gunyah-guest-SWITCHBACK.md`; 48 → 64 % in 25 min with the VM running).
- **Design chosen: (b)**, the DSP path, with one difference from the plan: piano's DSP firmware does not speak the Qualcomm
  battery manager protocol of `ShengBattmgr.c` but Xiaomi's "MCA" property protocol on the same channel. The transport is
  UniXNU-sheng's (SMEM, SMP2P, GLINK, AOSS QMP, RPMh, PAS, QRTR locator, FAT reader), the codec and policy are new. The battery
  itself is read from its two fuel gauges, which hang on this processor's I2C buses, so battery state needs no DSP at all.

## 1. Purpose and gate

Keep the tablet charging and report the battery while macOS runs natively. Minimum viable: input current at the stock 5 V
levels (9 V opt-in), charge current conservative, battery state on the console; IOPMPowerSource (menu bar) is the next step.

## 2. Hardware facts

| Item | Value | Source | State |
|---|---|---|---|
| Charger PMIC | PMIH010x, SID 7, REVID type 0x51 model 0x47/0x56; charger = lowest TYPE 0x02 SUBTYPE 0x7f/0xa8/0xba; DCDC +0x100, USBIN +0x300, Type-C +0x600 (each +0x300 for SUBTYPE 0xba–0xc4) | Project-SunUEFI `PianoUsbTypec.c` (BSD, from stock PmicDxe) | verified (code) |
| Port state | Type-C +0x06 sink status (Rp: bit 3 default USB, bit 2 1.5 A, bit 1 3.0 A), +0x08 source status, +0x0b bit 0 attached, bit 1 CC; +0x44 mode | same; the Rp order from Linux's `qcom_pmic_typec_port.c` register facts (`usb-host.md` §facts) | verified (code, source); the order was reversed here until 2026-11-11 |
| Fuel gauges | 2 × bq27z561 @0x55: pack A on `i2c@a94000` (qup1_se5, gpio52/53, with the SC8541 @0x6a), pack B on `i2c@88c000` (qup2_se3, gpio12/13); packs in parallel | live DT; SunUEFI `kernel.md` | verified (DT); SDA = first pin inferred |
| DSP | `remoteproc@6800000` sm8750-adsp-pas, PAS 1 + DTB 36, `qcom/sm8750/piano/adsp.mdt` + `.b01–.b54` (no b27), `adsp_dtb.mdt` + `.b01`, from `modem_a` `image/` | live DT; `xiaomipad-8pro-mainline/firmware/README.md` | verified |
| Carveouts | adspslpi 0xb8200000 + 0x5780000, q6-adsp-dtb 0x9ed80000 + 0x80000 (outside XNU's 0x8c0000000 window) | live DT, `el1-hyp-notes.md` §3 | verified |
| SMEM / SMP2P / IPCC | SMEM 0x81d00000 + 2 MiB, TCSR mutex 3; items 443/429, pid 2; IPCC 0x406000 (LPASS 3: GLINK 0, SMP2P 2; AOP 0) | live DT | verified |
| AOSS / RSC / cmd-db | QMP RAM 0xc300000 + 0x400; apps RSC drv-2 0x16520000, TCS +0xd00, active TCS 0–2; cmd-db 0x81c60000 | live DT | verified |
| Charger protocol | `PMIC_RTR_ADSP_APPS`, owner 0x800a: request {owner, type 1, op 1 read/2 write, property, seq, data[256]}, response adds retcode; type 2 = notification; USB-C port owner 0x800c (enable 0x10, ack 0x11 per notification 0x16) | SunUEFI `piano_mca.c` (spec only) | verified (source); 0x800c from Linux's header, needs-boot |
| Properties | ICL 0x2008 mA, FCC 0x2005 mA (its read is a no-op), charge enable 0x2003 (1 byte), Tbat 0x202a °C, bus V/I 0x20002/0x20004 (µV/µA), real type 0x2000c, PDOs 0x21011, fixed PDO request 0x21018 (OS3 numbering) | same | verified (source) |
| DSP needs | the AP's QRTR service locator for its charger domain, else the properties never answer | same, `qcom_pd_mapper` note | verified (source) |

## 3. Design: PianoPower

`package/PianoPower`, one IOService on the platform expert, one kernel thread; every step logs `PianoPower: ...`.

- **Level 1, read only.** SPMI arbiter → charger layout and the owner EE of every SID 7 APID → SMEM survey of the DSP (APPS/ADSP
  partition, SMP2P "slave-kernel" bits, GLINK items, crash reason) → both gauges. Then every 30 s: pack voltage (mean), current
  (sum, + = charging), state of charge (by full-charge capacity), temperature (the warmer pack), each pack's status word; the
  Type-C state (attached, source Rp seen = charger present) with the raw Type-C/USBIN/DCDC/charger status bytes; the DSP's
  SMP2P bits; the PMIC RTC's count and date (`rtc: ...`, [rtc.md](rtc.md), with every report once IOBSD is published). Writes: since
  2026-11-12 evening the charger watchdog's only (at both levels, `piano-power-wdog`, default 2: a pet every quarter of the bark time
  and bit 7 of MISC +0x53, "a bite disables charging", cleared, each write logged with before/after; `=1` pets only, `=0` none:
  [charging-policy.md](charging-policy.md) §18), otherwise none to any PMIC register; the SPMI observer channel's command; the four
  gauge pins' TLMM configuration (GPIO, pull-up, output value 0, output enable toggled: open drain, never driven high), restored after
  each read. Every report adds a `wdog:` line (the watchdog's configuration, the CHGR state, the pets) and the `charging:` line.
- **Level 2, charging.** Level 1 plus `piano_adsp_start` (after `piano-power-delay`, sheng's order in numbered steps, §5a:
  firmware from modem_a via NativeUfs + FAT → SMP2P item 429 → doorbells → AOSS `load_state adsp` → lcx/lmx at max through an
  idle active TCS of RSC drv-2 → DTB image PAS 36 → DSP image PAS 1 → ready on SMP2P (5 s) → votes dropped at handover → GLINK
  items 478/479/480 → `IPCRTR` with the locator and `PMIC_RTR_ADSP_APPS`). Doorbells and the RSC completion are polled (2 ms
  during bring-up, 20 ms after): the native KC has no SPI attach. SCM through Sm8750Scm.
  Then `PianoMca.c` reads 9–11 properties every 5 s (and on every notification), one in flight, 1 s timeout, and after each cycle
  applies the policy. Only four properties can ever be written (allowlist checked before a request is built):

  | Write | When | Value |
  |---|---|---|
  | ICL 0x2008 | attached and read back > 50 mA off | SDP 500, CDP 900, DCP/HVDCP 1500, PD min(5 V PDO, 1500) or 900 without PDOs, else 500; 9 V on: min(9 V PDO, 1600); detach: 100 |
  | FCC 0x2005 | wanted value changed | 1000 mA at 15–38 °C (1500 at 9 V), 500 outside or without a temperature (DSP's, else the gauges'); detach: 500 |
  | charge enable 0x2003 | reads 0 | 1, at most 3 times per attach, never 0 |
  | fixed PDO 0x21018 | only `piano-power-hv=1`, PD source with a fixed 9 V PDO, 3 cycles after attach, no authentication running | 9000; back to 5000 if the bus is not at 8.0–9.6 V within 3 cycles |

  Refused limit writes are tried 3 times in a row, then left until the next attach. USB-C port notifications are acknowledged.
  If no input limit write has taken 3 minutes after the DSP start, a WARNING line says the input may be held at 100 mA.
  **With `piano-power-policy=1`** ChargePolicy replaces this table ([charging-policy.md](charging-policy.md) §8): after charger_pd is
  up and the board configuration is through, the class rows (CDP 900 mA, DCP 1500, PD its 5 V offer), the charge current by class
  with −300 mA/°C above 38 °C, PianoThermal's board thermistors, re-sent on a new generation and after a wake, the sleep and
  shutdown handovers; `piano-power-rp=1` the Type-C Rp current, in steps up to 3 A while the bus holds since 2026-11-12
  ([charging-policy.md](charging-policy.md) §15; `piano-power-ramp=0`: at once, ≤ 1600 mA), `piano-power-health=1` stop at 80 %,
  resume at 75 %. A write's answer is awaited 3 s (`piano-power-write-ms`), a late one still taken. Since 2026-11-12 afternoon
  (built, not run): the charging restart when both packs stay at 0 mA with input to spare (0x2003 = 0 then 1, `piano-power-kick`,
  default on), 1500 mA charge current under a trusted Rp, and a `charging:` balance line every report
  ([charging-policy.md](charging-policy.md) §17).
- Not done (later): IOPMPowerSource, the SC8541 pump and PPS, MiPPS authentication, the OTG config write 0x201a (stock
  re-sends it to stop a DSP crash on dock unplug), float voltage/termination, sleep notifications.

Boot arguments: `piano-power=0|1|2` (default 0: probe declines, nothing mapped), `piano-power-period=S` (30),
`piano-power-gauge=0`, `piano-power-write=0` (level 2 without writes), `piano-power-hv=1`, `piano-power-fw=DIR`,
`piano-power-policy=1` (with `piano-power-rp=1`, `piano-power-ramp=0|1`, `piano-power-rp-max=MA`, `piano-power-write-ms=MS`,
`piano-power-health=1`, `-health-stop=N`, `-health-resume=N`, `piano-power-kick=0|1`: charging-policy.md §8, §15, §17),
`piano-power-wdog=0|1|2` (the charger watchdog: read only, pet, pet and clear the bite's charging stop (default); §18), and for the DSP
bring-up (§5a): `piano-power-delay=S` (30: seconds before the bring-up), `piano-power-stop=N` (stop before step N),
`piano-power-rpmh=0` (no lcx/lmx proxy votes), `piano-power-qmp=0` (no AOSS load_state), `piano-power-trace=0`.

## 4. Build (done here) and the kernel collection

```sh
tooling/k27/build-native-piano-power.sh        # host tests (SPMI model, I2C/bq27z561 model, MCA codec + policy) -> out/native-kexts/PianoPower
mkdir -p out/k27/extra && sed "s#@OUT@#$PWD/out#" tooling/k27/kext/PianoPower.json > out/k27/extra/PianoPower.json
PIANO_EXTRA_KEXTS=$PWD/out/k27/extra/PianoPower.json tooling/k27/build-native-image.sh   # KC + out/ram + out/ram-dump (inert image)
for L in 1 2; do PIANO_EXTRA_ARGS="piano-power=$L" PIANO_MENU_TIMEOUT=0 PIANO_RESET_AFTER=300 python3 tooling/bootimg/build-ram-images.py \
  --fd out/fd/piano-uefi-A-bundled.fd --fd-name A-bundled --out out/ram-power/level$L --kc out/k27/kc-k27-native.bin \
  --trust out/inputs/kernel.macho.trustcache --seed out/inputs/scratch-data.img --adt out/adt/piano-k27.adt; done
```

`build-native-image.sh` maps `PianoPower` to `build-native-piano-power.sh`; `configs/k27-native-kexts.txt` is unchanged (add the
line only once level 1 has run). Built 2026-11-11 in the agent worktree (kernel `kernel-k27-0059-native.macho`): test KC
sha256 `ec5267bc9e40a74e…` (`com.unixnu.driver.PianoPower: linked, header at 0xfffffff007099d90, 395 pointers`), level 1 image
`04860697c1759d7f…`, level 2 image `8ca4fae047e8fe86…` (verify-ram-images PASS); rebuild in the main checkout before a run.

## 5. Hardware test plan (owner present, on the cable, RAM boots only)

1. **Level 1** (`out/ram-power/level1`). `fastboot getvar battery-voltage`, then `tooling/native/native-loop.sh
   out/ram-power/level1/boot-piano-macos-ram-A-bundled.img`, `grep PianoPower logs/native-runs/<ts>-loop/xnu-console.txt`, then
   `battery-voltage` again. Expected (the numbers are examples):
   ```
   PianoPower: start: piano-power=1 period 30 s, gauges on; level 1: the DSP is not started; the only PMIC writes are the charger watchdog's (piano-power-wdog)
   PianoPower: spmi: arbiter version 0x7..., N APIDs
   PianoPower: charger PMIC (SID 7): N peripherals, owned by EE0 a, EE1 b, EE2 c, ...
   PianoPower: charger: PMIH010x model 0x47, charger block 0x..00 (subtype 0x.., APID n, owner EE m), ... Type-C 0x..00 (...)
   PianoPower: adsp: survey: SMEM APPS/ADSP partition found, items 0x200, slave-kernel -1 (no SMP2P state from the DSP: not running), ...
   PianoPower: gauge A: bq27z561 answers: 3900 mV, 120 mA, 55 %, 2700/4900 mAh, 31.0 C, status 0x.., N cycles
   PianoPower: [30s] battery: 3900 mV, 240 mA (charging), 55 % (5400/9800 mAh), 31.0 C, 2 of 2 packs | A ... | B ...
   PianoPower: [30s] usb: a source is attached (charger present) on CC1, Rp bits 0x.. (... mA), mode 0x.. | typec ...
   ```
   Pass: both gauges answer with plausible values; the port shows the Mac as a source; the charger's owner EE is logged. The
   **sign of the battery current on the cable is the measurement** that decides whether level 2 is needed at idle. Also run one
   session on a wall charger: boot, unplug the Mac, plug the charger, plug the Mac back before the reset (`PIANO_RESET_AFTER=600`).
2. **Level 2, no writes** (`piano-power=2 piano-power-write=0`, short session). Pass: `adsp: stage 11: DSP ready`, `channel
   PMIC_RTR_ADSP_APPS up`, `qrtr: the DSP asks the locator ...: answered`, `first charger reading: mca: bus ... type ...`.
   Expect the input at 100 mA (the DSP's default) during this test.
3. **Level 2** (`out/ram-power/level2`). Expected after step 2's lines: `mca: a source is attached: 5000 mV, type SDP|CDP|DCP|PD`,
   `mca: write input limit (0x2008) = 1500: ok (0)`, `mca: write charge current (0x2005) = 1000: ok (0)`, then reports with
   `input limit 1500 mA (set 1500)` and a positive battery current. Unplug/plug: `the source is gone ...` and the writes again.
4. Later: `piano-power-hv=1` on a 9 V PD charger (`asking the PD source for its fixed 9 V PDO`, `9 V on`, limit 1600 mA).

Fails safe: RAM boot; `piano-reset-after` resets; without `piano-power` nothing is mapped.

## 5a. Level 2 hang (2026-11-11 06:22) and the bisecting build

**What the log shows.** The run (`exec-log=1 piano-usb=2 piano-power=2 piano-power-write=0`) stopped after
`adsp: rails: lcx index 7, lmx index 4` and `adsp: stage 7: lcx/lmx votes`; the tablet was back in ABL 119 s after `fastboot boot`.
Both lines are printed only after the vote returned: the TCS reported completion (the line has no "done bit not seen" suffix) and
its finish writes were done. So the RPMh vote and the AOSS `load_state` before it completed; the hang is in what follows, which
logged nothing: the DTB image's metadata buffer and its UFS read, then the first PAS call (`init_image`, PAS 36), `mem_setup`, the
carveout writes, `auth_and_reset`. PianoDwc3's takeover (`piano-usb=2`) stopped at the same moment ("core soft reset") and is the
other candidate. k27 0056 pets the Gunyah watchdog from a high-priority thread call every 10 s, and the watchdog bites 32 s after
the last pet, so thread calls stopped on every CPU: a whole-system stall (a CPU stuck in the secure world holding something the
others need, or a bus access that never completes), not one blocked thread. The battery was unaffected (4476 → 4479 mV).

**The questions asked, answered from the DT and the Linux drivers as the spec** (rpmh-rsc, qcom_aoss, qcom_scm, qcom_q6v5_pas):
- RSC/DRV: `rsc@16500000` has drv-0/1/2 and `qcom,drv-id = <2>`: the HLOS's DRV is drv-2 at 0x16520000, TCS block +0xd00, done
  interrupt SPI 5 (level). `qcom,tcs-config` = active 3, sleep 2, wake 2, control 0, active first: TCS 0–2 are ours to use.
- TCS sequence (rpmh-rsc): an idle active TCS (DRV_STATUS non-zero), per command MSGID (8-byte length code, bit 8 = response
  required, bit 16 = write), address, data; WAIT_FOR_CMPL and CMD_ENABLE; the TCS bit in IRQ_ENABLE; CONTROL trigger off, AMC mode
  off, AMC mode on, AMC mode + trigger. With the response bit the RSC reports completion only after RPMh acknowledged the
  command, so a vote can wait for an ACK; that wait was bounded (100 ms) and it completed in the failed run.
- AOSS QMP: the message is written to the AOP's mailbox, the AOP is rung (IPCC AOP signal 0) and the sender waits for the length
  word to go back to zero; the interrupt only wakes the waiter early, so polling (as here) is enough. Step 6 completed.
- Do we need the votes? Linux takes lcx/lmx as proxy votes at the highest level until the DSP's handover; K90 (SM8750 Xiaomi,
  XNU) does the same and starts its DSP. Nothing shows that the boot stages hold LCX/LMX up; `piano-power-rpmh=0` answers it.
- Facts from the K90 port (`hackios-xnu-kernel-k90` annibale_adsp.c, APSL-2.0, read for facts only): on SM8750 XNU at EL1 under
  Gunyah reached "ADSP authenticated and READY" with the same DTB carveout 0x9ed80000, PAS 0x24 then 1, the v3 TCS layout at
  0x16520000 (idle TCS chosen by DRV_STATUS), `load_state adsp on`, and PAS calls issued only after the GPU's ZAP load so no two
  secure calls overlap. So PAS from XNU is possible on this SoC; piano differs in carveout (0xb8200000), in Sm8750Scm's wait-queue
  handling, and in running the bring-up while other drivers start.

**This build** (`package/PianoPower/PianoAdsp.cpp`): the bring-up is 16 numbered steps, each announced with `step N (...): begin`
plus a 20 ms sleep (so the console has the line before the access) and `step N done`; every secure call has `smc: ...: calling`
and `smc: ...: ok|FAILED (status, x0, x1, SMC count, interrupted, wait-queue, busy, µs)`; with `piano-power-trace` (default on)
every RSC and IPCC register write has a line before (`rsc: write CONTROL (AMC mode on + trigger) (+0x...) = 0x...`) and after
(`rsc: written ..., reads 0x...`). The vote follows the Linux sequence above, picks an idle TCS (logs DRV_STATUS/CONTROL/CMD_ENABLE
of TCS 0–2), polls IRQ_STATUS and each command's CMD_STATUS (issued/completed) for at most 100 ms, leaves a TCS that did not
complete alone, and restores IRQ_ENABLE. The bring-up waits `piano-power-delay` (30 s) first, so the root mount and PianoDwc3's
takeover are done before it touches anything.

| Step | What | Hardware touched |
|---|---|---|
| 3 | firmware opened | NativeUfs reads of modem_a (FAT) |
| 4 | SMP2P item | SMEM item 429 under TCSR mutex 3 |
| 5 | doorbells | IPCC RECV_ENABLE ×3, SEND |
| 6 | AOSS load_state (`piano-power-qmp`) | AOSS message RAM, IPCC |
| 7 | lcx/lmx votes (`piano-power-rpmh`) | cmd-db read, RSC drv-2 TCS |
| 8 / 13 | DTB / DSP metadata | contiguous buffer, UFS read, cache clean |
| 9 / 14 | DTB / DSP PAS init_image | SMC |
| 10 / 15 | DTB / DSP PAS mem_setup (relocatable images) | SMC |
| 11 / 16 | DTB / DSP segments | carveout 0x9ed80000 / 0xb8200000 mapped write-combined, written |
| 12 / 17 | DTB / DSP PAS auth_and_reset | SMC |
| 18 | DSP ready | SMP2P polling (5 s) |

Candidate images, built 2026-11-11 from main d03030d + this change (KC `e909bc79d3e03ffa…`, `com.unixnu.driver.PianoPower: linked,
402 pointers`, `native kext rule: 8 native kexts linked, none from outside this repository`), each `exec-log=1 piano-usb=2 piano-power=2
piano-power-write=0` plus:

| Image | Extra | sha256 | Answers |
|---|---|---|---|
| A | `piano-power-stop=9` | `87529659a623d644…` | steps 3–8 only, no secure call: if the system keeps reporting until the reset, the hang is in a PAS call or later |
| B | none | `da3605d9bce108db…` | the full bring-up, traced: the last `begin`/`calling`/`write` line names the hanging access |
| C | `piano-power-rpmh=0` | `69393043511cf356…` | the same without the proxy votes |

Run A first, then B; C if B hangs after step 7 or the DSP never becomes ready. Build and run (in the main checkout):

```sh
tooling/k27/build-native-image.sh                     # KC (PianoPower is in configs/k27-native-kexts.txt) + out/ram-dump
for V in "A piano-power-stop=9" "B" "C piano-power-rpmh=0"; do set -- $V
  PIANO_EXTRA_ARGS="exec-log=1 piano-usb=2 piano-power=2 piano-power-write=0 ${2:-}" PIANO_MENU_TIMEOUT=0 PIANO_RESET_AFTER=300     python3 tooling/bootimg/build-ram-images.py --fd out/fd/piano-uefi-A-bundled.fd --fd-name A-bundled --out out/ram-power/$1     --kc out/k27/kc-k27-native.bin --trust out/inputs/kernel.macho.trustcache --seed out/inputs/scratch-data.img --adt out/adt/piano-k27.adt
done
tooling/native/native-loop.sh out/ram-power/A/boot-piano-macos-ram-A-bundled.img   # dump image: out/ram-dump (inert: reset after 1 s)
grep 'PianoPower' logs/native-runs/<ts>-loop/xnu-console.txt
```

Expected in A, about 30 s after `PianoPower: stage 4: DSP start requested`: `adsp: waiting 30 s before the bring-up`, then
`step 3 (firmware opened): begin ...` … `step 7 done: lcx/lmx votes`, `step 8 (DTB metadata): begin: DTB`, `metadata: ... bytes at
0x...`, `step 8 done`, `step 9 (DTB PAS init_image) not started: piano-power-stop=9`, and the 30 s reports continue until
`piano-reset-after`. In B after step 8: `smc: PAS init_image (DTB, PAS 36): calling` then either `...: ok (...)` and the next
steps, or nothing (the secure call is the hang).

## 5b. Image B: the stall is at the first access to the DTB carveout (2026-11-11)

**Result.** A: steps 3–8, stopped at 9, reports to the reset. B: `smc: PAS init_image (DTB, PAS 36): ok (... 2 SMCs: 1
interrupted ...; 16720 us)`, `PAS mem_setup (DTB, 0x9ed80000 + 0x27000): ok (812 us)`, `carveout 0x9ed80000 + 0x80000: mapping
write-combined`, `segment 1: 159164 bytes from the file, 0 zero, at +0`, then the whole-system stall (ABL after 119 s). What ran
after the last line: the segment's UFS read into the bounce buffer, then the memcpy into the carveout.

**The questions, from the DT, the Linux drivers as the spec and the K90 port's facts:**
- *Is 0x9ed80000 the HLOS's?* Yes by every record: `q6-adsp-dtb@9ed80000` (0x80000, `no-map`) is an ordinary HLOS reserved-memory
  node inside the DDR range Gunyah gives the HLOS (`docs/hyp-evidence/memory-map-running-dt.txt:47`; the Gunyah-owned and VM
  regions are listed apart); our UEFI marks it `PIL_Reserved` (0x89E00000 + 0x19700000, uncached), so no cache lines of it are
  left; XNU's RAM is the 0x8c0000000 window. Nothing assigns it away at boot that we know of.
- *Linux's order:* the DTB is `memory-region[1]` of `remoteproc@6800000`, mapped write-combined at probe (`ioremap_wc`); the load
  callback runs `qcom_mdt_pas_init` (init_image, then mem_setup of the span) and then `qcom_mdt_load_no_init` (the segment
  writes): the writes come after mem_setup, as here. There is no `qcom_scm_assign_mem` or hypervisor call for the SM8750 ADSP
  (only for the CDSP's global sync memory, SunUEFI patch 0015). The K90 port writes the same DTB carveout (0x9ed80000) after its
  mem_setup through a write-combined mapping, byte by byte, and its DSP becomes READY.
- *The mapping type:* XNU's write-combined is MAIR 0x44, Normal non-cacheable (`osfmk/arm64/proc_reg.h`), the same type as
  Linux's `ioremap_wc`; so the type matches Linux. Differences left: memcpy's wide stores (K90 stores bytes), mapping 0x80000
  where mem_setup declared 0x27000 (a mapping makes no access, but it is now limited to the span), and the UFS read just before.

**This build:** a probe of the carveout's first 8 bytes (an aligned read, then the same value written back, each announced
and confirmed) at the end of step 8 (`probe (before init_image)`, no secure call yet) and at the start of step 11 (`probe (after
mem_setup)`); `segment N: ... bytes read from the file; storing them` after the UFS read and `first ... bytes stored` after the
copy; the copy and zero fill as aligned 8-byte stores (no memcpy/bzero, no DC ZVA); the mapping limited to the span;
`piano-power-map=1|2` (Device-nGnRE / Device-nGnRnE instead of write-combined) and `piano-power-load-first=1` (segments written
before init_image and mem_setup).

| Image | Extra | sha256 | Answers |
|---|---|---|---|
| D | none | `e88d9228b2abbd6f…` | which access stalls: probe before init_image (read/write), probe after mem_setup (read/write), UFS read, the stores |
| E | `piano-power-map=1` | `3759dbb8db046787…` | the same with a Device-nGnRE mapping |
| F | `piano-power-load-first=1` | `4dd9e407b475966a…` | the segments written before any PAS call touches the region |

KC `1ef0cb65d480fe4d…` (main 7457964 + this change; `native kext rule: 8 descriptors, all this repository's`, `8 native kexts
linked, none from outside this repository`), each image `exec-log=1 piano-usb=2 piano-power=2 piano-power-write=0` plus the extra; verify PASS.
Run D; E or F only if D stalls after `probe (after mem_setup)` passes or at its write. Build in main as in §5a with
`"D" "E piano-power-map=1" "F piano-power-load-first=1"` as the variants. Reading D:

| Last line | Meaning |
|---|---|
| `probe (before init_image): 8-byte read at 0x9ed80000 ...: begin` | the carveout is not readable by the HLOS at all |
| `probe (before init_image): read ...; writing it back: begin` | readable but not writable before any PAS call |
| `probe (after mem_setup): ... read ...: begin` / `... writing it back: begin` | mem_setup took the read / write access away (then F) |
| `segment 1: 159164 bytes read from the file` missing after `segment 1: ...` | the UFS read stalled, not the carveout |
| `segment 1: ... storing them` with no `first ... stored` | the stores stall although one word went through (then E) |

## 5c. Level 2 with writes drains: the input path, not the limit (2026-11-11 07:28)

**Result** (owner present, Mac USB-C port = CDP). Before the DSP: `battery: ... 0 mA (idle), 98 %` — the system ran from the USB
input. The DSP's first charger reading: `bus 5231 mV 0 mA, type CDP, input limit 100 mA (set 0), charge enable 1, status 0x7`. Our
writes were accepted (`write input limit (0x2008) = 900: ok`, `write charge current (0x2005) = 1000: ok`, read back as
`input limit 900 mA (set 900)`), and yet `[65s] battery: 4440 mV, -860 mA (discharging)` with `bus 5233 mV 0 mA` still.

So the limit is not the gate: it was accepted and read back, charging is enabled, the bus is at 5.2 V, and no input current flows.
Something upstream of the limit holds the input off once the DSP's firmware owns the path (before the DSP the same port fed the
system). Candidates, each a property of the same class in the Linux driver's family (Project-SunUEFI `piano_mca.c`, GPL, read as a
specification) and in the K90 port's notes (APSL, facts only): the input-suspend flag `0x2019`, the automatic input-current
limiting `0x2012`/`0x2029` (it walks the limit down when the source sags), and the DSP's USB-C port notifications, which its
Type-C/PD state machine waits on (Ubuntu's `pan_ack`; we acknowledge them, but no notification was logged in that run). The
charger status read `0x7`, which is none of the five values those sources name, so our decode of `0x2001` may be wrong too.

**This build, read only first (image G).** At level 2 with `piano-power-write=0` the cycle also reads `0x2019` (input suspend),
`0x2012` (AICL enable) and `0x2029` (AICL status), and a `diag:` line after every cycle of the first minute (then with each
report) gives those three, the decoded `0x2001`, the bus current, the input limit, and how many port notifications arrived and
were acknowledged. Nothing new is written. One `diag:` line, abbreviated:

```
PianoPower: [40s] diag: input suspend (0x2019) 0 (not suspended), AICL enable (0x2012) 1, AICL status (0x2029) 0x30,
  charger status (0x2001) 0x7 (unknown), bus current (0x20004) 0 mA, input limit (0x2008) 900 mA;
  port notifications 0 received, 0 acked, enable sent; 13 reads, 0 errors, 0 timeouts, 0 ignored, 0 notifications
```

**The one write it can make (image H, `piano-power-unsuspend=1`).** When `0x2019` reads non-zero, one byte 0 is written to it, at
most three times per attach, read back on the next cycle, logged as `write input suspend (0x2019) = 0: ok (0)`. The allowlist
only ever permits the value 0 there, never 1, so this driver cannot suspend the input. Everything else is unchanged; CDP keeps the
Ubuntu policy's 900 mA (its table value) until a run shows the limit matters. If `0x2019` already reads 0, the write never
happens and the `diag:` line says so — then the gate is AICL or the port notifications, not the suspend flag.

**DRV-2 lock.** Every rail vote of ours now runs with PianoA830's `sm8750_rpmh_drv2_owner` held (compare-exchange 0 for `POWR`,
10 us between tries, at most 2 s, released after the TCS is reclaimed), so `piano-power>=2` and `piano-gpu=2` can run together.
PianoA830 is listed before PianoPower in `configs/k27-native-kexts.txt` and `PianoPower.json` names it as a library, so the KC
linker (which has no weak imports) resolves the word to A830's single copy — checked in the KC's symbol list. `piano-power-rpmh=0`
still skips our votes, which answers whether they are needed at all.

| Image | Arguments (plus `exec-log=1 piano-usb=2`) | sha256 |
|---|---|---|
| G (probe, no new writes) | `piano-power=2 piano-power-write=0` | `4db6e479469844fa…` |
| H (the one write) | `piano-power=2 piano-power-unsuspend=1` | `3766b18882dc4cc0…` |

KC `83cda10d09a6bd43…` (main 1774c13 + this change; `native kext rule: 9 native kexts linked, none from outside this repository`), both verify
PASS. Commands (in the main checkout):

```sh
tooling/k27/build-native-image.sh
PIANO_EXTRA_ARGS="exec-log=1 piano-usb=2 piano-power=2 piano-power-write=0" PIANO_MENU_TIMEOUT=0 PIANO_RESET_AFTER=300 \
  python3 tooling/bootimg/build-ram-images.py --fd out/fd/piano-uefi-A-bundled.fd --fd-name A-bundled --out out/ram-power/G \
  --kc out/k27/kc-k27-native.bin --trust out/inputs/kernel.macho.trustcache --seed out/inputs/scratch-data.img --adt out/adt/piano-k27.adt
PIANO_EXTRA_ARGS="exec-log=1 piano-usb=2 piano-power=2 piano-power-unsuspend=1" PIANO_MENU_TIMEOUT=0 PIANO_RESET_AFTER=300 \
  python3 tooling/bootimg/build-ram-images.py --fd out/fd/piano-uefi-A-bundled.fd --fd-name A-bundled --out out/ram-power/H \
  --kc out/k27/kc-k27-native.bin --trust out/inputs/kernel.macho.trustcache --seed out/inputs/scratch-data.img --adt out/adt/piano-k27.adt
tooling/native/native-loop.sh out/ram-power/G/boot-piano-macos-ram-A-bundled.img
grep -E 'PianoPower: (\[|diag|mca|first)' logs/native-runs/<ts>-loop/xnu-console.txt
```

Run G first and read its `diag:` lines; H only if G shows `input suspend (0x2019) 1 (SUSPENDED)`. With either image, watch the
`battery:` line: the gate is found when the battery current stops being negative on the cable.

**Not done, and why.** The property that un-suspends the input is not provable from the licence-clean sources alone; piano's own
stock charger modules would name it exactly, but recovering IDs from those proprietary binaries is a vendor-binary derivation this
repository does not do (§8). `0x2019` is the candidate those sources agree on, which is why it is the one opt-in write.

## 5d. The battery for macOS (`piano-power-ps=1`)

**What.** With `piano-power-ps=1` (off by default) PianoPower publishes the battery to macOS: an `IOPMPowerSource` subclass named
`AppleSmartBattery` (`package/PianoPower/PianoBattery.cpp`), attached under PianoPower and refreshed on each report cycle
(30 s) with `updateStatus()`. It works at level 1: it needs only the two gauges and the Type-C state, no DSP, and writes no
hardware. The values (`BatteryMap.h`, host-tested in `tests/battery_host.cpp`):

| Property | From |
|---|---|
| `BatteryInstalled` | a gauge reading |
| `CurrentCapacity` / `MaxCapacity` | the packs' charge as a percentage (remaining over full, weighted) / 100, as on current Macs |
| `AppleRawCurrentCapacity`, `AppleRawMaxCapacity`, `NominalChargeCapacity`, `DesignCapacity` | mAh, both packs summed (`DesignCapacity` from bq27z561 0x3c, else the full charge) |
| `Voltage`, `Amperage`, `InstantAmperage` | mean pack voltage; summed average and instantaneous current, negative while discharging |
| `Temperature` | the warmer pack, hundredths of a degree Celsius |
| `CycleCount` | the higher pack's |
| `ExternalConnected`, `ExternalChargeCapable` | a source's Rp on the Type-C port (else, without the port view, a charging battery) |
| `IsCharging`, `FullyCharged` | on power and the packs take current / both report FC or 100 % |
| `TimeRemaining`, `AvgTimeToEmpty`, `AvgTimeToFull` | minutes from the charge and the average current; 65535 when it cannot be computed |
| `AdapterDetails` | Type-C family, 5 V, the Rp current, watts |
| `AtWarnLevel`, `AtCriticalLevel` | always false |

**How macOS finds it** (xnu source, `iokit/Kernel`): `IOPMrootDomain` and powerd look for services of class `IOPMPowerSource`;
`updateStatus()` copies the values into the registry and sends `kIOPMMessageBatteryStatusHasChanged`. The class is named
`AppleSmartBattery`, the name a Mac's battery service has, so a lookup by that name finds it as well; the vma2 kernel collection
has no class of that name (checked: its strings name only `IOPMPowerSource` and `IOPMPowerSourceList`). The service is published
only after the first gauge reading, with its values already in the registry, so macOS never sees an empty battery at boot.

**Low charge.** Nothing here sets the warning or critical levels; macOS's warnings follow the percentage. The kernel's own
low-battery sleep starts from `kIOPMPowerEmergency`, which only Apple's battery manager sends, and every system sleep request
passes `checkSystemSleepEnabled()`, which refuses it while the ADT's `/defaults sleep-disabled` is 1 (`configs/k27-adt.toml`; it
is 1 in every native image). So a low battery gives macOS's notices but no sleep attempt; sleep stays untested natively.
`piano-power-ps-floor=N` (at most 50) shows at least N % while on battery, for a session where even the notices are unwanted;
the console line marks a clamped value `(floor)`. The tablet itself still cuts off at the real empty point: watch the
`battery:` line, not the menu bar, in long sessions.

**Test image** (owner present or the installed config; in the main checkout):

```sh
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_MENU_TIMEOUT=0 PIANO_RESET_AFTER=300 \
  PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-power=1 piano-power-ps=1" tooling/k27/build-native-image.sh
tooling/native/native-loop.sh out/ram/boot-piano-macos-ram-A-bundled.img
grep -E 'PianoPower: (piano-power-ps|battery: AppleSmartBattery|\[[0-9]+s\] ps:)' logs/native-runs/<ts>-loop/xnu-console.txt
```

Expected lines (numbers from the level 1 measurement):

```
PianoPower: piano-power-ps=1: the battery is published to macOS as AppleSmartBattery (IOPMPowerSource) from the gauges and the Type-C state, refreshed every 30 s
PianoPower: battery: AppleSmartBattery (IOPMPowerSource) published: 99 %, on power, not charging
PianoPower: [0s] ps: 99 %, on power, not charging, 4479 mV, 0 mA (instant 0), 8522/8634 mAh (design ...), 114 cycles, time remaining 65535 min (unknown)
PianoPower: [30s] ps: 99 %, ...
```

Visible result: the menu bar's battery item at the gauges' percentage with the plug symbol while the Mac's cable is in (and
Control Centre's Battery entry); unplugged, the percentage with "Power Source: Battery" and a time estimate once the current is
steady. `ioreg -rn AppleSmartBattery` (if a shell is available) lists the properties above. Built 2026-11-11 on main 7c04081:
KC `dba24c1a1e8d814a…` (`11 native kexts linked, none from outside this repository`, PianoPower 719 pointers), image verify PASS (its own hash
changes with every build: it carries the build time as `droidvm-utc`). Not booted.

## 5e. What the Ubuntu history adds (images I and G2, 2026-11-11)

`docs/ubuntu-history/device.md` §2.5/§3.1 and `docs/ubuntu-history/kernel.md` §2.5/§3.1 (the Ubuntu device and kernel history; the
GPL kernel read for facts only) change how the 07:28 drain reads and what level 2 sends:

- **Linux saw input current with no host write at all**: an SDP port at 5.041 V gave **394 mA** with the DSP running while 0x2008
  read 100 and the type read 1 (K:1e4a0fc89fee). Linux never wrote 0x2019 or 0x2012. So the DSP's defaults do let a computer port
  charge; H (the 0x2019 write) stays opt-in and image I does not write 0x2019.
- **0x2008 reads back the requested limit, not the one in force**, and **0x20004 reads 0 once the firmware considers charging done**.
  Our run was at 98 %. The verdict is the gauges' summed current (now on the `diag board:` line); next runs should be at 40–90 %
  and on a wall DCP/PD charger as well as the Mac's port: Ubuntu has no CDP result at all, and only one SDP reading.
- **Every client waits for `msm/adsp/charger_pd` to report up**: PianoQrtr.c now registers a listener with the DSP's service
  registry notifier (QMI service 0x42, instance 74; `Servreg.h`, message layout from Linux's PDR messages) and acknowledges every
  state change; the charger codec, the port enable, the UCSI probe and the policy start only on "up" (or 60 s after the channel
  without an answer, logged), and every new "up" (a new service generation) sends everything again: the firmware is back on its
  defaults, and Ubuntu #18 recovered only by re-sending.
- **The stock board configuration** per generation, before any policy write, in the stock order: 0x201c = 1 byte 1 (the SC8541
  charge pump is present), then 0x201a = 16 bytes (words 0, 1, 0xffffffff, 0xffffffff), then 0x201b = 0x708 (the firmware's
  cable-insertion detection id) (K:cf4d132fdee4, K:27490ca3d1f4, K:de5a8c2b874f). `PianoMca.c` board(): read back on the diag line,
  retried for five cycles, the allowlist takes exactly these three values; `piano-power-board=0` leaves it out. Linux added it in
  v0.5.0 for OTG, after sink charging already worked, so it is not proven to matter for input current.
- **The three owners**: battery/charger 0x800a (Xiaomi's MCA properties), UCSI 0x800b, USB-C port (PAN) 0x800c. Our PAN encoding
  already matched Linux (request {owner 0x800c, type 1, opcode 0x15, cmd, arg, reserved}: cmd 0x10 = enable with arg 0, cmd 0x11 =
  ack with arg = port; notifications opcode low byte 0x16, the SVID in bits 31:16; every write answered with opcode 0x15). What
  differed: we enabled before charger_pd was up, and gave the answer 1 s, not 5 s. Now the enable goes after "up", the answer gets
  5 s, and the outcome is logged (`port notifications enabled` / `no answer from owner 0x800c`): no answer means that owner is not
  served at all.
- **UCSI, read only**: Linux runs a full UCSI client on every "up"; we run none. Both images now read the UCSI buffer once per
  generation (owner 0x800b, opcode 0x11, the 12-byte header alone; the answer carries VERSION and CCI) and log UCSI notifications
  (opcode 0x13), sending nothing back. It answers whether the owner is served; whether the DSP's Type-C state machine waits for a
  UCSI host is *open* (kernel.md §3.1). `piano-power-ucsi=0` leaves the read out.
- **The DSP's first QRTR HELLO is kept**: the DSP may send as soon as it holds one of our intents, before the channel reports up
  here; Linux's GLINK dropped that first HELLO in 1 cold boot of 26 and then had no service at all (K:235ab34e2a6e). PianoQrtr.c
  used to clear the router's state when the channel came up, which would have discarded such a HELLO; it now clears it when the
  channel goes down, keeps what came first (`qrtr: the DSP's hello came before the channel's up ... kept`) and sends our HELLO
  on "up" if it has not gone out yet.
- **The firmware's view of the port**, read every cycle (`diag port:`): 0x21002 verify (adapter authentication in progress; an
  interrupted one left 5 V on the bus with both cells slowly discharging, Ubuntu #20), 0x2100d PD state (2 = SNK_Ready), 0x2100c
  data role (1 UFP, 2 DFP), 0x200d/0x200e OTG boost/gate, 0x21020 OTG plug-in, 0x21015 Type-C mode, 0x21021 CC toggle, 0x21022
  sink/source mode, 0x20001 online. A DSP that believes it sources, or is stuck authenticating, shows here.
- **DSP restarts and handover**: a FATAL and a lost "ready" are logged with a count and the crash reason (Ubuntu #3: a sensor
  loop restarted the ADSP about 8 times a minute; natively nothing serves the sensors' files). Nothing restarts the DSP natively
  yet; a FATAL means the charger is back on its defaults. The piano ADSP raises handover again after every power collapse
  (K:9adeb3213d24); ours was already one-shot (the proxy votes go once), the re-raises are now counted and the slave-kernel bit
  changes are logged for the first 16, then every 64th.
- **The float voltage default is 4400 mV** (0x2007 reads 5200; §0 corrected), shown decoded on the `diag board:` line.

Expected new lines (level 2; the values are what Linux saw or what is to be found):

```
PianoPower: qrtr: the DSP said hello (node 5)       (or: the DSP's hello came before the channel's up (node 5, N services): kept)
PianoPower: servreg: the notifier is at 5:N (instance 0x4a01); registering a listener for msm/adsp/charger_pd: sent
PianoPower: servreg: msm/adsp/charger_pd is up (0x1fffffff, registration answer)
PianoPower: mca: PMIC_RTR_ADSP_APPS up: waiting for msm/adsp/charger_pd to report up
PianoPower: mca: msm/adsp/charger_pd is up: reading the charger properties
PianoPower: ucsi: reading the buffer of owner 0x800b (read only)
PianoPower: ucsi: owner 0x800b answers: version 1.0, CCI 0x..., 48-byte buffer (UCSI 1.x), return code 0   (or: no answer in 5 s)
PianoPower: mca: USB-C port notifications enabled: owner 0x800c answered the enable (opcode 0x15, cmd 0x10)
PianoPower: mca: write CP state (0x201c) = 1: ok (0)                (image I only, these four)
PianoPower: mca: write OTG configuration (0x201a) = 0: ok (0)       (the 16-byte constant)
PianoPower: mca: write CID configuration (0x201b) = 1800: ok (0)
PianoPower: mca: board configuration taken (CP state, OTG configuration, CID): the policy follows
PianoPower: [45s] diag port: verify (0x21002) 0, PD state (0x2100d) ..., data role (0x2100c) ..., ..., online (0x20001) 1
PianoPower: [45s] diag board: CP (0x201c) 1, CID (0x201b) 0x708, OTG (0x201a) 0, sent 0x7; float (0x2007) 5200 = 4400 mV;
  port enable answered; UCSI answered; charger_pd up (generation 1); battery (gauges) ... mA
```

| Image | Arguments (plus `exec-log=1 piano-usb=2`, built with `PIANO_GPU=2 PIANO_VMM_PRESENT=0`) | What it answers |
|---|---|---|
| G2 | `piano-power=2 piano-power-write=0` | no charger write: the charger_pd wait, the port enable after "up" (answered or not), the UCSI read, both new diag lines |
| I | `piano-power=2` | the same plus the board configuration, then the policy (ICL/FCC by type, CDP 900 mA); no 0x2019 write |

Both take PianoA830's DRV-2 lock for our rail votes, so they run with `piano-gpu=2` (`PIANO_GPU=2` adds `piano-gpu=2
piano-gpu-delay=20`). Built here on main 7d9f10e + this change (`native kext rule: 14 native kexts linked, none from outside this repository`;
KC `d2a8fb802274b151…`, G2 `a941253fed1c9bfe…`, I `7914e7c9889b4435…`, both verify PASS; the hashes change with every build,
which embeds `droidvm-utc`). With `PIANO_GPU=2` the seed is the A830-staged one, not the installed seed, so main's store guard
builds both with `data-persist=0` (Data volatile, the installed store untouched). Commands (main checkout):

```sh
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_MENU_TIMEOUT=0 PIANO_RESET_AFTER=300 \
  PIANO_EXTRA_ARGS="exec-log=1 piano-usb=2 piano-power=2 piano-power-write=0" tooling/k27/build-native-image.sh
mkdir -p out/ram-power/G2 && cp out/ram/boot-piano-macos-ram-A-bundled.img out/ram/build-A-bundled.json out/ram-power/G2/
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_MENU_TIMEOUT=0 PIANO_RESET_AFTER=300 \
  PIANO_EXTRA_ARGS="exec-log=1 piano-usb=2 piano-power=2" tooling/k27/build-native-image.sh
mkdir -p out/ram-power/I && cp out/ram/boot-piano-macos-ram-A-bundled.img out/ram/build-A-bundled.json out/ram-power/I/
tooling/native/native-loop.sh out/ram-power/G2/boot-piano-macos-ram-A-bundled.img out/ram-dump/boot-piano-macos-ram-A-bundled.img
grep -E 'PianoPower: (qrtr|servreg|ucsi|mca|\[[0-9]+s\] (diag|battery|mca))' logs/native-runs/<ts>-loop/xnu-console.txt
```

Run G2, then I, each at 40–90 % and once on a wall charger; the gauges' current on the `diag board:` and `battery:` lines is
the verdict. Reading G2: charger_pd never "up" → the gate is the PD itself (the 60 s fallback line follows); `port enable NO
ANSWER` or `UCSI NO ANSWER` → that owner is not served; `verify` non-zero or `sink/source`/`data role` showing a source → the DSP
is not sinking; all normal with the gauges still negative → image I (the board configuration) is next, then a UCSI client.

## 6. Risks

- **Level 2 can charge less than no driver.** Once the DSP runs, its firmware holds the input at 100 mA until the ICL write takes
  (the 11:11 drain: about -1.8 A per cell under load). If GLINK, the locator or the properties fail, only the WARNING line shows it;
  reboot. A PMIC is not reset by a warm reset: after a failed level 2, check `battery-voltage` before trusting the next boot.
- **PAS from XNU** is verified on SM8750 for the GPU's ZAP (13T, K90) and for the DSP on the K90; on piano the first attempt
  stalled the whole system (§5a). A secure-world hang ends in the Gunyah watchdog reset about 30 s later. Since 2026-11-11 no
  secure call of another kext can run inside a PAS sequence or alongside one of ours: every request and each image's whole
  sequence (`startImage`) hold the kernel-wide secure-call lock (`../scm-lock.md`), PianoA830's word.
  Sm8750Scm's limit of 10000 SMC instructions per request could cut a long authentication short (logged as a status).
- **Memory:** the top 0x50000 of the adspslpi carveout overlaps our UEFI's DXE heap range (gone after ExitBootServices); the
  images are written there only in level 2.
- **TLMM:** the gauge pins are switched to GPIOs and back every report; a wrong SDA/SCL assignment gives "no ack" lines, not
  damage (open drain only). The SC8541 shares bus A and is never addressed.
- **Protocol assumptions:** owner 0x800c for port notifications; PD ids of the OS3 firmware (HyperOS 4 moves 0x2100b–0x21026 by
  one: keep `piano-power-hv` off with other firmware; the §5e diag reads 0x2100c, 0x2100d, 0x21015 and 0x21020–0x21022 are
  Linux's ids and would show a neighbouring property there, harmless as reads but misleading); FCC cannot be read back; the Rp
  bit meaning.
- **Board configuration (image I):** three writes of the stock values Linux and the stock driver send on every DSP start; the
  allowlist takes nothing else. With 0x201a the DSP itself sources VBUS when a USB device (Rd) is attached (Linux never turns VBUS
  on; `usb-host.md`'s H3 step relies on it): with an OTG adapter or a bus-powered device on the port, image I powers it.
- **UCSI read (both images):** a read of the PPM's buffer, no UCSI command; a firmware that starts its UCSI machine on the first
  read would then send notifications nobody answers (logged, the first 8).
- **Thermal:** nothing beyond the DSP's JEITA window and the 500 mA floor at ≥ 38 °C; no charge-pump or fast charging.

## 7. Open questions

1. The PMIC's state without the DSP on piano: idle current on the Mac port and on a charger (level 1).
2. Owner EE of the charger peripherals (level 1 log) and whether any is owned by EE 0.
3. Whether the DSP needs more than the locator before its charger domain answers (level 2 step 2).

## 8. Licence note

Transport from UniXNU-sheng (BSD-2-Clause-Patent) and Sm8750Scm from Adreno830 (MIT), hashes in `package/PianoPower/UPSTREAM`.
`piano_mca.c`, xiaomi_mca, pmic_glink, qcom_battmgr, qcom_q6v5_pas and bq27xxx (GPL-2.0) are specification only; PianoMca.c,
Gauge.h and ChargerPmic.h are written fresh. Xiaomi's own charger modules (the `mca_*`, `qcom_subpmic_proxy` and
`qcom_adsp_pd_protocol` binaries in the stock `vendor_boot` ramdisk) are **not** used: they are proprietary, and recovering
property ids or register maps from them by disassembly is outside what this repository derives from. Property ids come from the
GPL driver's text and the APSL K90 port's notes, both read as descriptions, and are confirmed or refuted on the device.

## 9. The DSP client contract (PianoAudio, PianoSensors, PianoUsbHost, PianoDp)

The DSP that charges also hosts the audio and sensor domains, and natively nothing restarts it: one DSP crash ends charging until
the reboot. So PianoPower stays its only owner and offers the other kexts a client interface,
`package/include/PianoAdspContract.h`. Each client names `com.unixnu.driver.PianoPower` as a library in its descriptor and is
listed after PianoPower in `configs/k27-native-kexts.txt`, so the KC links the functions by name to PianoPower's single copy.

| Offered | What | Answers `sensors.md` §10.1 / `audio.md` §10 |
|---|---|---|
| GLINK channels | `piano_adsp_channel` / `piano_adsp_send` (sheng's interface): **6 slots** (IPCRTR, PMIC_RTR_ADSP_APPS, then 4 for clients: `adsp_apps`, `fastrpcglink-apps-dsp`, 2 spare). PianoPower's two are always kept free, whatever registers first. A name can have only one owner. Messages up to 1056 bytes, a 16-message queue per channel. | A |
| QRTR client ports | `piano_qrtr_open(service, instance, client, ctx)` with `server(up, node, port, instance)` and `receive(node, port, data, len)` callbacks; `piano_qrtr_server` (with a generation); `piano_qrtr_send` (queued, ≤ 1024 bytes); `piano_qrtr_close` (DEL_CLIENT to the DSP). Up to 8 ports, numbered from 16; a reopened slot gets a new number. A port can be opened before the DSP runs. IPCRTR now has **2048-byte receive buffers** (8 of them). | B |
| Health | `piano_adsp_health(&h)`: stage, `dsp_up` (ready, not fatal, GLINK agreed and not broken, router hello), the fatal bit, the fatal-report count, the **link generation** (moves when a channel that was up goes down, or the transport breaks), `charger_pd` state and generation, the quiet bits, and **its own crash-reason string** (no longer shared with the `open:` notes). | C |
| Quiet | `piano_adsp_quiet()`: `PIANO_ADSP_QUIET_CHARGER`, set while a charger generation is configured. It runs from the codec's link until a whole cycle ends with nothing left to write and the port enable is answered or timed out (board configuration and first policy writes included), at most 60 s. It is advisory: clients still answer what the DSP waits for. | C |
| Port events | `piano_port_events(client, ctx)` with `event(ctx, PianoPortEvent *)`, `piano_port_seen()` (`PortEvents.c`, 2026-11-11, for PianoDp's D2): every USB-C port notification (owner 0x800c, opcode 0x16) handed to up to **4 clients** as the bytes received (≤ 32 kept, with the real length and a sequence), on PianoPower's thread after the acknowledgement is queued; read only: nothing goes back to the DSP, PianoPower stays the port owner's only correspondent. Without a client: one atomic load. Clients taking more than 20 ms are logged. Host test `tests/port_events_host.cpp` | — |
| USB-C relay (UCSI) | `package/include/PianoUsbcContract.h` (2026-11-11 night, for PianoUsbHost's UCSI client, `usb-host.md` §6.6): the UCSI owner 0x800b lives on PMIC_RTR_ADSP_APPS, not on the router, so no QRTR port reaches it. One client: `piano_usbc_attach` (`state(ready, generation)`, `ucsi()`), `piano_usbc_send` (queued, at most 4; **only** UCSI reads and writes of owner 0x800b pass, never 0x800a, never the port owner 0x800c), `piano_usbc_detach`, `piano_usbc_status`. Ready = the generation linked, the quiet down and PianoPower's own UCSI read through; a new generation restarts the client. The port notifications reach the same client through the port events above. | — |

**The relay in PianoPower** (new files `UsbcRelay.c`, `PianoUsbcRelay.cpp`; five small hooks in `PianoPower.cpp`, none in its policy
or suspend paths): the receive loop offers each message to the relay before PianoMca (a UCSI message is the client's alone while it is
ready; everything else, port notifications included, goes on to PianoMca and the port events), `piano_usbc_relay_step` after each pass makes the state
call and sends the queue through PianoPower's own channel, the wait does not block while a send is pending. Without a client nothing
changes (PianoMca sees every message as before); `tests/usbc_host.cpp` covers the request check, the queue, the ready transitions and
the order of the two.

**What protects the charger from a client.**
- Every channel has its own buffers.
- The DSP thread sends one message per channel per round, so a busy client channel never holds the charger's behind it in the
  shared FIFO.
- On the router channel, client packets are refused while fewer than 4 of its 16 queue slots would stay free; those are for
  PianoPower's servreg and locator answers.
- No lock is held across a client callback. The router step's events are collected under the router lock and delivered after it
  is dropped. Each is delivered only while its port is open, with the port marked busy, so a close from another thread waits for
  the callback in progress; a close from inside the port's own callback does not wait.
- A callback that takes more than 20 ms is logged, because the charger waited with it.
- Nothing in the contract starts, stops or restarts the DSP, votes rails, or touches RPMh, AOSS or PAS.

**Late registration.** A client kext may register its channel after the DSP has opened it. Before, such an open was only noted,
and the channel never came up. GLINK now keeps up to 4 such opens and answers one when its channel is registered (`late_opens`).
It forgets one that the DSP closes, and when a fifth arrives it drops the oldest.

**Without clients** nothing changes but the logs: at level 2 there are `quiet: raised …` and `quiet: released …` lines per
charger generation, and the `adsp:` report line gains `link generation N, quiet 0x0, client ports 0 (…)`.

Host tests are in `tests/ports_host.cpp`: the port table against the router fed with the DSP name service's packets, the router's
12 local ports, DEL_CLIENT and the 1024-byte packet limit, the channel slots, the health pieces, the quiet flag, the charger codec's
`configured`, and GLINK answering a late registration.

## Measured 2026-11-11 (level 1, read only, 240 s on the Mac's USB-C port)

- SPMI arbiter v7, charger PMIC SID 7 = PMIH010x model 0x56; charger/DCDC/USBIN/Type-C blocks owned by EE 0, which is this processor (the arbiter node's `qcom,ee = <0>`; see §0).
- DSP: SMEM APPS/ADSP partition present, no SMP2P state: not running, as expected natively.
- Both gauges answer (bq27z561 on qup1_se5 gpio52/53 and qup2_se3 gpio12/13): 4480/4479 mV, 99 %, 4287/4344 and 4235/4290 mAh,
  28–30 °C, 113/114 cycles.
- Type-C: a source attached on CC1, Rp 0x2: **3.0 A** (logged as "500 mA" by the decode of the time, whose bit order was the
  reverse of the PMIC's; corrected 2026-11-11 from Linux's register facts, `usb-host.md` §facts). Battery current 0 mA ("idle")
  for the whole run: at 99 % the system ran from the USB input without draining the pack. Whether a partly discharged pack charges without the DSP is still to be measured
  (needs a lower charge level); level 2 (DSP start) is for an owner-attended session.
- ABL `getvar battery-voltage` before/after: 4478 / 4477 mV.

## Measured 2026-11-11 07:15 (level 2, image D, piano-power-write=0, owner present)

The full DSP bring-up works from native macOS: DTB and DSP segments stored (aligned 8-byte stores; the earlier image B hang
writing segment 1 did not recur), `PAS auth_and_reset (DSP, PAS 1): ok` (229 ms, 21 SMCs), SMP2P ready, GLINK version agreed, a
channel up, the IPC router hello from node 5 (servreg/pddump/pdr lookups answered), `PMIC_RTR_ADSP_APPS up`, and the first charger
reading: `bus 5236 mV 0 mA, type CDP, input limit 100 mA (set 0), charge current set 0 mA, tbat 29 C, charge enable 1, status 0x7`.
Next: the same with writes (input limit 1500 mA). Do not combine piano-power>=2 with piano-gpu=2 yet (both use RPMh DRV-2 TCS
slots; the A830 driver takes the DRV-2 owner lock, PianoPower does not).

## Measured 2026-11-11 evening (the Mac's USB-C port, level 1 then level 2 without writes)

From the USB console log of the 20:22 run (local, not published; `grep PianoPower`) and the level 1 run before it (the day's
short record: [results-2026-11-11.md](../results-2026-11-11.md)):

| What | Level 1 (no DSP) | Level 2, image G2 (DSP running, no writes) |
|---|---|---|
| Battery (gauges, both packs) | **+477 mA at 97 %**: the PMIC charges by itself on the boot stages' configuration | 0 mA at 98 % before the DSP; −1.2 A at its start; **−1.6 to −1.7 A at the login screen** (4.35 V, 97 %, packs up to 37.7 °C) |
| Input | not readable (no DSP) | bus 5208 mV, **397 mA** (0x20004; 287–397), type **CDP**, 0x2008 reads **100 mA** (the request, not the limit in force) |
| Input path | — | input suspend 0x2019 = 0, AICL enable 0x2012 = **0**, AICL status 0x2029 = 0x64, charge enable 1, status 0x7, float 5200 (4400 mV), verify 0, PD state 1, data role 1 (UFP) |
| DSP services | — | charger_pd up (registration answer), port owner 0x800c answered the enable, UCSI owner 0x800b answered (version 1.10, CCI 0, 48-byte buffer) |
| Type-C (PMIC) | a source on CC1, sink status 0x02 | the same: **Rp 3.0 A** (the run still printed the old decode, "500 mA") |

Reading: the input path is open with the DSP running (unlike the 07:28 run), the firmware holds it near 400 mA, and with the
system drawing about 9 W (2.1 W in, 7.1 W from the packs) the battery drains. Without the DSP the same port charged. So level 2 needs
its input limit written, and the limit is now worth writing: [charging-policy.md](charging-policy.md) §13 (the CDP row by default,
the Rp current opt-in, and the budget: 900 mA in is about −1.1 A on the packs at 9 W, 1600 mA about −0.4 A) and §14 (images P1 and
P2 with their expected lines: the input limit write ok, the bus current above 397 mA, the packs' current toward 0). P1 asks image
I's question (the board configuration, then 900 mA on the CDP) through ChargePolicy; either answers it.

## Measured 2026-11-12 (the night's queue on the Mac's USB-C port: level 1 in 16 boots, level 2 in I, P1, P2, pol-chargesleep, usbhost-u0)

From the USB console logs (local, not published; `grep PianoPower`). **M** = measured, **I** = inferred.

| | Level 1 (no DSP: the PMIC alone) | Level 2, before the first write | Level 2, 0x2008 = 900 (I, P1) | Level 2, 0x2008 = 1600 (P2, Rp trusted) |
|---|---|---|---|---|
| Bus (firmware's 0x20002 / 0x20004) | not readable | 5.22–5.23 V, **397 mA** (M) | 5.04–5.05 V, **897 mA** (M) | 4.89–4.93 V, **1453–1594 mA** (M) |
| 0x2008 / 0x2029 | — | 100 / 0x64 (M) | 900 / 0x384 (M) | 1600 / 0x640, passing 0x4e2 a second after the write (M) |
| Input | about **1.4–1.5 A** (I: the power balance below; equal to BC 1.2's CDP current) | ~2.1 W | ~4.5 W | ~7.8 W |
| Packs at the idle login window, 89–92 % | **about −75 mA** steady, 0 to −195 mA (M; 0 mA at 97 % on float; −0.7 to −1.7 A with the uncapped governor, pol-observe) | −1.2 to −1.7 A (M) | −0.68 to −0.92 A (M, P1) | **−173 → −46 mA** (M) |
| Packs early in the boot (light load) | **+472 to +480 mA**, flat, in 12 of the 16 boots at 89–97 % (M; the other 4 started on float at 0 mA): the PMIC's own charge current (I) | — | — | — |

- **Why level 2 holds the input near 400 mA at first** (M): with no host write the firmware's request is its power-on 100 mA
  (0x2008 reads 100, 0x2029 0x64) and the bus reads 397 mA (287–397; Linux saw 394 mA on an SDP, `ubuntu-history/kernel.md` §2.5).
  It ignores the port's CDP type and its 3.0 A Rp on its own: those are a host client's to act on (Linux's `charge_policy=1`).
  (I) The ~400 mA under a 100 mA request is the firmware's hardware floor. After a write the bus follows the request within 6 mA
  (900 → 897, 1600 → 1594), so 0x2008 is the lever and the firmware does take it on this port.
- **The input at level 1** (I): the PMIC runs on the configuration the boot stages left; its input is not readable (no DSP, no
  register map: the `usbin`/`chgr` status bytes ChargerPmic.h reads are all zero). At the login window level 1 and P2 (1594 mA) end at
  the same packs' current (−63..−90 vs −46 mA) under the same load, so level 1's limit is about 1.5 A; early in the boot the packs
  take a flat ~477 mA whatever the charge level: the charge current the PMIC regulates to by itself. Its AICL and type detection are
  the hardware's; nothing here can read or set them.
- **What the system draws** (I, from P1 and P2 at ~90 % conversion): about **7.5 W** at the idle login window (P1: 4.1 W from the
  input + 2.9–4.0 W from the packs; P2: 7.0 W + 0.2–0.75 W), with PerfPowerServices and audiomxd spinning a core (power.md 1.2, P11)
  and cluster 0 at UEFI's 1996 MHz; about **10–11.5 W** with the governor at 3.5 GHz (pol-chargesleep). Without the spinning daemons
  it should be lower; ordinary use adds the GPU and the apps.
- **0x2005 "no answer"** (M): the first charge current write after the board configuration (input still at 100 mA) timed out in the
  codec's 1 s window in P1 and P2, and in P2 the first input limit write too, although the next cycle read it back as written with
  0x2029 moving to it: taken and applied, the answer late or lost. Writes made after the input limit (I, usbhost-u0) and a charge
  current of 500 (pol-chargesleep) were always answered at once; no read ever timed out. Changes and the test that tells late from
  lost: [charging-policy.md](charging-policy.md) §15.3.
- **The level 1 SPMI write** (M, every boot): `charger: PMIH010x model 0x56, charger block 0x2200 (subtype 0xba, APID 698, owner EE 0),
  DCDC 0x2600 (APID 702, EE 0), USBIN 0x2800 (APID 704, EE 0), Type-C 0x2b00 (APID 707, EE 0)`: the arbiter would let this processor
  write them. Not done: there is no licence-clean register map of this PMIC's USBIN/charger blocks (mainline's SCHG maps are other
  generations' subtypes; piano's stock charger driver runs in the DSP and its binaries are not read here), and a guessed power-path
  write can suspend the input or brown the tablet out; the gain would be what level 2 gets through 0x2008 with read-backs.
  Since 2026-11-12 evening one exception, the charger watchdog's pet and its "a bite disables charging" bit, behind gates and an
  allowlist of two MISC registers ([charging-policy.md](charging-policy.md) §18).
- **What follows**: the stepped Type-C current up to the Rp's 3 A with its guards, the write answers, the test images and the daily
  profile's level: [charging-policy.md](charging-policy.md) §15 and §16.

## Measured 2026-11-12 afternoon (chg-ramp, chg-l1, chg-late, p8, p9: why the battery reads 0 mA on the cable)

From the USB console logs (local, not published) and a survey of every native boot of 11-11 and 11-12 with PianoPower; the whole
analysis, the fix and its images: [charging-policy.md](charging-policy.md) §17.

- **The battery is really at 0 mA (M):** both gauges and the firmware's own pack current (0x2027) read 0, the remaining capacity does
  not move for minutes, while the same reads move the voltage and the temperature; BatteryStatus 0x40 is the gauge's DSG bit
  (discharge or relax mode, not charge mode), set ~55 s after the current reaches 0, with no FC or TCA bit.
- **The input is not used up (M):** `chg-ramp` 7.2 W in (1334-1709 mA at 4.87-4.98 V under 2000 asked, the firmware's own 0x2029
  1750-1800), `chg-late` 7.1 W (under 1600): the system about 6.4-6.5 W (I, at 90 %), the bus current swinging by up to 375 mA while
  the packs stay at 0. The steps stopped at 2000 because the input was never used up.
- **The charger stops charging at about 40 s of every native boot (M):** 0 of 42 level-1 boots that logged past 45 s had the packs
  above +50 mA after it (+476 mA before, in 36 of 46); 0 of 16 at level 2, whatever the input limit; `p9-off`, with every CPU
  98.7-99.9 % idle, 0 mA at 82 % from 70 s to 890 s. At 5 s resolution: 477 → 463 → 328 → 0 mA over 28-38 s with no discharge, the
  DCDC block's status bytes changing in the same 5 s. The DSP starts after it (charger_pd up at 60-75 s) and keeps the input on and
  the battery at 0: charging enable 1, input suspend 0, the charge current written, 4400 mV float, 30-35 °C.
- **Inferred:** a not-charging state of the PMIC charger latched at ~40 s of uptime that the firmware neither reports nor clears (a
  watchdog left by the boot stages, or a termination/inhibit decision: no register map to tell); not excluded, that this firmware does
  not charge from a computer port at all (no Ubuntu result on one).
- **Evening (I):** the block at 0x2600 ("DCDC" above) is the charger's CHGR in the SCHG family's layout (Type-C − 0x500), and its
  +0x06 goes `c3`/`83` → `87` at the stop: state full-on → **disabled** (not terminated or inhibited). The charger watchdog (MISC
  0x2c00, never read so far) is the lead; PianoPower now reads it, pets it and clears "a bite disables charging" by default
  ([charging-policy.md](charging-policy.md) §18).
- **Fix built (not run):** the charging restart (0x2003 = 0, 5 s, 1, the charge current again; three per attach), 1500 mA under a
  trusted Rp, the steps against a settled 0x2029, the `charging:` line; images `ram-chg-kick` and `ram-chg-kick-idle`
  (charging-policy.md §17.5); if the restart does not help, the attended next steps of §17.6 (a replug, an early DSP, a wall charger).

## Measured 2026-11-12 night (dev14: the watchdog and the enable command read)

- **The watchdog is off (M):** MISC 0x2c00 `WD_CFG` 0x00 and +0x53 0x00 as the boot stages left them: no pet, no clear (`wdog: not
  armed`). The hypothesis of the evening fails ([charging-policy.md](charging-policy.md) §19.2, with its one caveat).
- **The enable command is on (M):** CHGR 0x2600 +0x42 = 0x05 (bit 0 set; the `cmd 04 45 05` of the `wdog:` line is +0x40..+0x42),
  CHGR_CFG2 +0x51 = 0x07 (the command register in control, active high) while the state byte reads 0x87 (disabled) from the first
  report: dev14 started disabled, a few minutes after a boot that had stopped, and so did dev8n and dev13w's second boot.
- **Not ours (M):** no driver in this repository writes SID 7 0x22xx-0x2fxx, and the read-only level-1 boots stop all the same.
- **Every charger block is mapped twice** in the arbiter, once for EE 3 (inferred: the DSP's channel) and once for EE 0 (ours).
- **Built (not run):** `piano-power-chgen` (default: set the enable command when it reads off; `=2`: a 2 s pulse of it when it reads
  on and the charger disabled) and the `chgr: changed since` record of every register change between two reports (§19.3-19.5).

## Measured 2026-11-12 late (the 21:18 RAM boot and the installed 21:31 boot: the stop's bytes)

- **The stop, recorded (M):** between two reports CHGR +0x06 c3/83 → 87 (full-on → DISABLED), +0x07 19 → 00, +0x0a 00 → 05, +0x0c
  (the charge current, 50 mA steps, I) 0a/14 → 00, rt 22 → 20; the enable command and CHGR_CFG2 unchanged; MISC +0x06 bit 6 follows
  the load and is no warning ([charging-policy.md](charging-policy.md) §20.2).
- **Decoded against the family's public layouts (I):** no byte names a reason beyond "disabled"; the input path (DCDC ICL 1500 mA,
  AICL done; USBIN plug-in) reads healthy in both states; no family timer is 30-60 s (§20.3).
- **New (M):** the two level-2 boots whose DSP was ready before the stop never stopped; every other one stopped before its DSP and
  stayed so: the early DSP (`piano-power-delay=0`) is the most promising next test.
- **Built (not run):** the whole complex logged and diffed with every report, the 2 s watch of the stop, the IORegistry picture for
  a shell (`PianoChargerText`, `PianoCharger`, `PianoChargerChanges`), and `piano-power-chgfix` (default 0; level 1) (§20.4-20.8).
