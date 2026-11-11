# Charging and battery: PianoPower (fuel gauges, the charger PMIC's port, the DSP's charger firmware)

Status 2026-11-11: `package/PianoPower` built and linked into a test KC (host tests pass); **not run on the tablet**. Inert unless
`piano-power=N`. Gate for making macOS the default OS (`droidvm-build/piano-patches/persistent-boot-plan.md` §3.2 and §4).

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
  Found`, `droidvm-build/piano-patches/logs/uefi-A-debug-222920/pianolog.txt:2001`; its config writes and watchdog enable are
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
  sources, and acknowledges the DSP's USB-C port notifications. The DSP itself keeps a 4350 mV float voltage and a 0–55 °C
  JEITA window. Measured: 8.8 V / 1.59 A in, +1.2–1.34 A per cell at light load (`streamC-charging-report.md`).
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
| Port state | Type-C +0x06 sink status (Rp bits 1–3), +0x08 source status, +0x0b bit 0 attached, bit 1 CC; +0x44 mode | same | verified (code); Rp bit→current inferred |
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
  SMP2P bits; the PMIC RTC's count and date (`rtc: ...`, [rtc.md](rtc.md), at start and with every report). Writes: none to any
  PMIC register; the SPMI observer channel's command; the four gauge pins' TLMM configuration
  (GPIO, pull-up, output value 0, output enable toggled: open drain, never driven high), restored after each read.
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
- Not done (later): IOPMPowerSource, the SC8541 pump and PPS, MiPPS authentication, the OTG config write 0x201a (stock
  re-sends it to stop a DSP crash on dock unplug), float voltage/termination, sleep notifications.

Boot arguments: `piano-power=0|1|2` (default 0: probe declines, nothing mapped), `piano-power-period=S` (30),
`piano-power-gauge=0`, `piano-power-write=0` (level 2 without writes), `piano-power-hv=1`, `piano-power-fw=DIR`, and for the DSP
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
   PianoPower: start: piano-power=1 period 30 s, gauges on; level 1: read only (no PMIC register written, the DSP is not started)
   PianoPower: spmi: arbiter version 0x7..., N APIDs
   PianoPower: charger PMIC (SID 7): N peripherals, owned by EE0 a, EE1 b, EE2 c, ...
   PianoPower: charger: PMIH010x model 0x47, charger block 0x..00 (subtype 0x.., APID n, owner EE m), ... Type-C 0x..00 (...)
   PianoPower: adsp: survey: SMEM APPS/ADSP partition found, items 0x200, slave-kernel -1 (no SMP2P state from the DSP: not running), ...
   PianoPower: gauge A: bq27z561 answers: 3900 mV, 120 mA, 55 %, 2700/4900 mAh, 31.0 C, status 0x.., N cycles
   PianoPower: [30s] battery: 3900 mV, 240 mA (charging), 55 % (5400/9800 mAh), 31.0 C, 2 of 2 packs | A ... | B ...
   PianoPower: [30s] usb: a source is attached (charger present) on CC1, Rp bits 0x.. (... mA, inferred), mode 0x.. | typec ...
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
402 pointers`, `native kext rule: 8 native kexts linked, none from DroidVM`), each `exec-log=1 piano-usb=2 piano-power=2
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
linked, none from DroidVM`), each image `exec-log=1 piano-usb=2 piano-power=2 piano-power-write=0` plus the extra; verify PASS.
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

KC `83cda10d09a6bd43…` (main 1774c13 + this change; `native kext rule: 9 native kexts linked, none from DroidVM`), both verify
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
| `AdapterDetails` | Type-C family, 5 V, the Rp current (inferred), watts |
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
KC `dba24c1a1e8d814a…` (`11 native kexts linked, none from DroidVM`, PianoPower 719 pointers), image verify PASS (its own hash
changes with every build: it carries the build time as `droidvm-utc`). Not booted.

## 6. Risks

- **Level 2 can charge less than no driver.** Once the DSP runs, its firmware holds the input at 100 mA until the ICL write takes
  (the 11:11 drain: about -1.8 A per cell under load). If GLINK, the locator or the properties fail, only the WARNING line shows it;
  reboot. A PMIC is not reset by a warm reset: after a failed level 2, check `battery-voltage` before trusting the next boot.
- **PAS from XNU** is verified on SM8750 for the GPU's ZAP (13T, K90) and for the DSP on the K90; on piano the first attempt
  stalled the whole system (§5a). A secure-world hang ends in the Gunyah watchdog reset about 30 s later.
  Sm8750Scm's limit of 10000 SMC instructions per request could cut a long authentication short (logged as a status).
- **Memory:** the top 0x50000 of the adspslpi carveout overlaps our UEFI's DXE heap range (gone after ExitBootServices); the
  images are written there only in level 2.
- **TLMM:** the gauge pins are switched to GPIOs and back every report; a wrong SDA/SCL assignment gives "no ack" lines, not
  damage (open drain only). The SC8541 shares bus A and is never addressed.
- **Protocol assumptions:** owner 0x800c for port notifications; PD ids of the OS3 firmware (HyperOS 4 moves 0x2100b–0x21026 by
  one: keep `piano-power-hv` off with other firmware); FCC cannot be read back; the Rp bit meaning.
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

## Measured 2026-11-11 (level 1, read only, 240 s on the Mac's USB-C port)

- SPMI arbiter v7, charger PMIC SID 7 = PMIH010x model 0x56; charger/DCDC/USBIN/Type-C blocks owned by EE 0, which is this processor (the arbiter node's `qcom,ee = <0>`; see §0).
- DSP: SMEM APPS/ADSP partition present, no SMP2P state: not running, as expected natively.
- Both gauges answer (bq27z561 on qup1_se5 gpio52/53 and qup2_se3 gpio12/13): 4480/4479 mV, 99 %, 4287/4344 and 4235/4290 mAh,
  28–30 °C, 113/114 cycles.
- Type-C: a source attached on CC1, Rp 0x2 (500 mA). Battery current 0 mA ("idle") for the whole run: at 99 % the system ran
  from the USB input without draining the pack. Whether a partly discharged pack charges without the DSP is still to be measured
  (needs a lower charge level); level 2 (DSP start) is for an owner-attended session.
- ABL `getvar battery-voltage` before/after: 4478 / 4477 mV.

## Measured 2026-11-11 07:15 (level 2, image D, piano-power-write=0, owner present)

The full DSP bring-up works from native macOS: DTB and DSP segments stored (aligned 8-byte stores; the earlier image B hang
writing segment 1 did not recur), `PAS auth_and_reset (DSP, PAS 1): ok` (229 ms, 21 SMCs), SMP2P ready, GLINK version agreed, a
channel up, the IPC router hello from node 5 (servreg/pddump/pdr lookups answered), `PMIC_RTR_ADSP_APPS up`, and the first charger
reading: `bus 5236 mV 0 mA, type CDP, input limit 100 mA (set 0), charge current set 0 mA, tbat 29 C, charge enable 1, status 0x7`.
Next: the same with writes (input limit 1500 mA). Do not combine piano-power>=2 with piano-gpu=2 yet (both use RPMh DRV-2 TCS
slots; the A830 driver takes the DRV-2 owner lock, PianoPower does not).
