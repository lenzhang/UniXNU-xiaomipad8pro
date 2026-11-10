# Charging and battery: PianoPower (fuel gauges, the charger PMIC's port, the DSP's charger firmware)

Status 2026-11-11: `package/PianoPower` built and linked into a test KC (host tests pass); **not run on the tablet**. Inert unless
`piano-power=N`. Gate for making macOS the default OS (`droidvm-build/piano-patches/persistent-boot-plan.md` §3.2 and §4).

## 0. The answer

- **Charging control on SM8750/piano needs the audio DSP (ADSP).** The buck charger is a block of the PMIH010x PMIC (SID 7) whose
  SPMI channels belong to the DSP's execution environment; this processor only gets the arbiter's read-only observer channel.
  Mu-Silicium's piano DXE list drops the stock PmicDxe because it "faults in SPMI on channel 690, which ABL no longer gives the
  apps EE" (`Mu-Silicium-piano/Platforms/Xiaomi/pianoPkg/Include/DXE.inc:61`). So design (a), a kext that writes charger
  registers over SPMI, is not available; PianoPower level 1 logs the owner EE of every SID 7 peripheral to confirm it.
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
  SMP2P bits. Writes: none to any PMIC register; the SPMI observer channel's command; the four gauge pins' TLMM configuration
  (GPIO, pull-up, output value 0, output enable toggled: open drain, never driven high), restored after each read.
- **Level 2, charging.** Level 1 plus `piano_adsp_start` (sheng's order: firmware from modem_a via NativeUfs + FAT → SMP2P item
  429 → doorbells → AOSS `load_state adsp` → lcx/lmx at max through RSC TCS 1 → DTB image PAS 36 → DSP image PAS 1 → ready on
  SMP2P (5 s) → votes dropped at handover → GLINK items 478/479/480 → `IPCRTR` with the locator and `PMIC_RTR_ADSP_APPS`). Doorbells
  and the RSC done bit are polled (2 ms during bring-up, 20 ms after): the native KC has no SPI attach. SCM through Sm8750Scm.
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
`piano-power-gauge=0`, `piano-power-write=0` (level 2 without writes), `piano-power-hv=1`, `piano-power-fw=DIR`.

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

## 6. Risks

- **Level 2 can charge less than no driver.** Once the DSP runs, its firmware holds the input at 100 mA until the ICL write takes
  (the 11:11 drain: about -1.8 A per cell under load). If GLINK, the locator or the properties fail, only the WARNING line shows it;
  reboot. A PMIC is not reset by a warm reset: after a failed level 2, check `battery-voltage` before trusting the next boot.
- **PAS from XNU** is verified only for the GPU's ZAP on the 13T; a secure-world hang ends in the Gunyah watchdog reset.
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
Gauge.h and ChargerPmic.h are written fresh.
