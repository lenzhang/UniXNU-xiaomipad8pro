# Charging and battery: ADSP battery manager over glink, Xiaomi MCA policy

Status: design, 2026-11-10. Nothing built or run. Template: `README.md`.

## 1. Purpose and gate

Keep the tablet charging and report the battery while macOS runs natively. Gates **any native session longer than the
battery** (plan R4): when `piano-charging.service` was stopped on Ubuntu, the cells drained at about -1.8 A per cell
even on the cable (`PROGRESS-thin-macos.md`, 11:11 incident). Minimum viable: the stock 9 V / 1.6 A buck path with the
battery reported to macOS; MiPPS/PPS fast charging later.

## 2. Hardware facts

| Item | Value | Source | State |
|---|---|---|---|
| Charger control | ADSP-mediated: the AP talks to the ADSP's charger/battery service over pmic_glink; Xiaomi's MCA relay (`xiaomi_mca`, MCA_PROP_* properties such as MCA_PROP_PDOS 0x21011, MCA_PROP_DATA_ROLE 0x2100c) on top | `streamC-charging-report.md`; `streamC2-auth-report.md` | verified |
| ADSP | remoteproc PAS (SCM), firmware from `/lib/firmware/qcom/sm8750/xiaomi/piano/`; reserved `adspslpi@b8200000` (87.5 MiB), `q6-adsp-dtb@9ed80000` | live DT (`docs/hyp-evidence/memory-map-running-dt.txt`) | verified |
| PD contract (test adapter) | 5 V/3 A and 9 V/3 A fixed only; no PPS APDO; 9 V at ICL 1600 mA (≈14 W), stock buck ceiling `xiaomi_mca_pd_limits = {1600, 3000}` | `streamC-charging-report.md` 1, 3 | verified |
| Direct charging | `sc8541` charge pump over I2C; needs a PPS APDO ≥ 8.7 V and ≥ 1.5 A, battery 3.5–4.1 V, 15–38 °C to start, exit 45 °C | `streamC-charging-report.md` Fix | verified (code) |
| Gauges | 2× `bq27z561` (two cells) | plan §4(d).4 | verified |
| Thermal staging | 42.5–44 °C | plan §4(d).4 | verified |
| MiPPS | Xiaomi SVID 0x2717 visible only after a PD **data-role swap to DFP**; the Ubuntu script checked the SVID before the swap (fixed in `xiaomi-mipps-auth-dfp-before-svid.diff`) | `streamC2-auth-report.md` 1–4 | verified |
| Behaviour without the AP policy | cells drain on the cable (the defaults the ADSP applies without `piano-charging`'s module parameters do not cover the load) | 11:11 incident | verified (once) |

## 3. Ubuntu as the specification

- `drivers/power/supply/xiaomi_mca.c` (linux-sm8750-piano v1.1.0, GPL-2.0): the property set, `xiaomi_mca_policy_poll`,
  `xiaomi_mca_restore_icl` (ICL back to 100 mA on detach/reattach, :938-963), `xiaomi_mca_cp_try_start` (:2006-) and
  `xiaomi_mca_cp_apdo` (:1919-1941), the 15 s HV auth wait (`MCA_HV_AUTH_WAIT_MS`), auth reset tolerance 30 s.
- `piano-charging.service`: sets module parameters once at boot (hv_charge, charge_current, full_charge, mipps_auth,
  cp_charge, pan_ack, charge_policy). These values are the policy the Darwin driver must apply.
- `/usr/libexec/xiaomi-mipps-auth` (+ the DFP-before-SVID diff): the authentication sequence, userspace.
- Stock reference: onyx `mca_pd_auth.c`, `qcom_adsp_pd_protocol.c` (cmd 200 = ADSP_VDM_REQ_INIT → PD_PROP_DATA_ROLE).

## 4. UniXNU template to build on

**UniXNU-sheng** 00ba909 `package/`: `AdspFirmware.c` (ADSP PAS load), `QcomSmem.c`, `QcomGlink.c` (glink over SMEM),
`ShengBattmgr.c` (pmic_glink battery manager client), `BatteryProbe.cpp` / `ShengBattery.cpp` (IOPMPowerSource), started
with `adsp=2` ("the battery on the ADSP" in the macOS 27.3 override); host checks `check-adsp-host.py`,
`check-qrtr-host.py`, `check-battery-host.py`. Piano delta: SM8750 ADSP PAS id/firmware names and reserved regions,
the Xiaomi MCA property set on top of battmgr, the `piano-charging` policy values, then (later) MiPPS with the DFP swap.

## 5. IOKit family / ABI

IOPMPowerSource (AppleSmartBattery-shaped properties: CurrentCapacity, MaxCapacity, IsCharging, ExternalConnected,
Voltage, Amperage, Temperature) so the menu bar and `pmset -g batt` work, as sheng's ShengBattery publishes.

## 6. Boot args

`adsp=2` (boot the ADSP and the battery manager, sheng's value), `battery=1`, `charge-policy=stock` (planned: the
`piano-charging` parameters), `mipps=0` until §8 step 4.

## 7. Kernel archive / component

sheng's ADSP/glink/battmgr sources as a piano board package (`package/PianoAdsp*`, `PianoBattery*`), or a shared
SoC-level component if UniXNU-sm8750-common takes them; GPU/audio later reuse the same glink.

## 8. Hardware test plan

After M2, on the cable, owner present, short sessions:
1. Read-only: SMEM present, ADSP state (already running from ABL? or must be loaded), glink channel list. Pass: logged.
2. ADSP up + battmgr: battery percentage and voltage logged and equal to Ubuntu's within 1 %. No writes to the charger.
3. Policy: apply the `piano-charging` values; measure input power and cell current (Ubuntu's `charging-sample.sh` figures:
   ≈8.8 V / 1.59 A input, cells +1.2–1.34 A at light load). Pass: cells charge at idle.
4. Later: MiPPS (DFP swap, SVID 0x2717), PPS, sc8541 direct charging, thermal staging.
Fails safe: RAM boot; on any doubt reboot to Ubuntu, which re-applies its policy at boot.

## 9. Fallback

Battery-only sessions, kept short (R4); charge in Ubuntu between runs.

## 10. Open questions

1. Is the ADSP already running when the HLOS starts (ABL/UEFI QcomChargerDxe keep charging), and does it stop or keep
   the last policy when Linux's pmic_glink goes away? (needs-boot; Ubuntu dmesg remoteproc lines first)
2. Which MCA properties are writes and which are reads; minimum write set for "stock 9 V/1.6 A". (source read)
3. SCM PAS from XNU at EL1 under Gunyah: same calls as Linux (`qcom_scm_pas_auth_and_reset`), expected to work as for the
   13T's ZAP shader (Adreno830 S7 "PAS 13"). (needs-boot)

## 11. Licence note

`xiaomi_mca.c`, ucsi, pmic_glink (GPL-2.0): specification only. sheng's ADSP/glink/battmgr code: check UniXNU-sheng
`LICENSE` before reuse; new code written fresh.
