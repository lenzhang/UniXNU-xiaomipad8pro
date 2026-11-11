# Hardware backlog: piano features Ubuntu proved, mapped to the macOS port (2026-11-11)

Rule (owner, 2026-11-11): if the Ubuntu project already made a piano-specific hardware feature work on this tablet, native macOS
probably needs it too. Scope: piano's own hardware only — not generic desktop features (VMs, Waydroid, Settings UX, release
notifications, dual-boot switching). Sources: the Ubuntu hardware table and `docs/STATUS.md` at v1.2.0
(yzddmr6/xiaomipad-8pro-mainline 7fe59cc), `docs/ubuntu-history/device.md` and `docs/ubuntu-history/kernel.md` (K: = kernel
commit, D: = device commit). Ubuntu is read for behaviour and facts only; everything is reimplemented here. No GPL code, no vendor
blobs, no other unit's calibration in this repository.

Standing constraints for every item:
- Opt-in boot argument first; host design and tests; then RAM-boot tests; the installed profile only after a pass.
- **The ADSP is shared** (charger firmware, audio, the SSC sensor hub). Only PianoPower starts it (`piano-power>=2`); no other
  driver starts, stops or restarts it, or votes its rails. Items that need the DSP are gated on PianoPower level 2 being healthy.
- Power work (battery, sleep, interrupts, charging, thermal) keeps priority; these items slot around it.
- Per-unit data (sensor registry/calibration in `persist`, touch calibration, MAC addresses) is read at run time or from the
  owner's private copy, never committed; `persist` is never written.

## 1. Already covered (in flight or done) — extend only where a gap is listed

| Ubuntu feature | Ours | Gap left |
|---|---|---|
| Internal display, screen off/on | NativeGopDisplay (GOP copy) + PianoBacklight (`piano-backlight=2`: levels, sleep off/on, lid opt-in) | real DPU path → B1 |
| Brightness | PianoBacklight steps + kernel brightness keys | auto-brightness → B3 |
| Touch, CSOT/BOE detection | PianoTouch (gpio100 + the unit's calibration panel id, firmware per panel, auto orientation) | multi-touch/gestures (single pointer today); pen not in Ubuntu either |
| Magnetic keyboard + touchpad, key wake | PianoKeyboard (keys, touchpad, gestures, adaptive polling); hall gpio80 read | wake from screen-off/suspend → B2 |
| Power / volume keys | PianoInput (Power 0x30, Vol± 0xE9/0xEA consumer usages, verified pressed on the tablet) | volume has no audio device to act on until audio lands; nothing else |
| Wi-Fi | PianoWifi levels 1–4 (survey → power → PCIe → MHI firmware) | WMI/QMI/scan/join: months (`wifi.md`) |
| USB device mode (console, HID, NCM) | PianoDwc3 | — |
| Battery gauges, battery temperatures, menu-bar battery | PianoPower level 1 + `piano-power-ps=1` | — |
| Charging | PianoPower level 1/2 (level 2 input stays 0 mA: image I in progress) | policies → B4 |
| RTC / time | k27 0060 `piano-rtc=1` + offset; NTP over USB NCM | — |
| CPU frequency, power modes, thermal | PianoThermal (`piano-thermal`, `piano-cpufreq=1..3`: Ubuntu's Power Saver/Balanced caps) | macOS Low Power Mode mapping → B5; PMIC board thermistors → B6 |
| GPU acceleration | PianoA830 + Turnip | idle power (in flight) |
| Suspend (s2idle, CX off) | k27 cpuidle/s2idle patch + power contract (in flight) | lid/keyboard/charging policies → B2, B4 |
| Speakers, microphones | PianoAudio level 1 + `audio.md` design | levels 2+ (6–9 weeks) |
| Boot time (Ubuntu: ~13 s to login) | `docs/boot-time.md`, `PIANO_FAST_BOOT` | ours ~150 s; keep cutting |

## 2. Backlog (new tasks)

| # | Feature (Ubuntu evidence) | macOS need | Depends on | Next step (offline first) | Tablet |
|---|---|---|---|---|---|
| B1 | **Native DPU display path**: refresh 60/90/120/144 Hz (Ubuntu default 120 Hz since v0.8.0, D:e070564), Night Light = DPU colour-temperature (PCC/GC) kept across off/on and suspend, panel off sequence (display-off, 20 ms, sleep-in, 100 ms, supplies off; K:72cfce4c86e5), dual-DSI (v0.9.x right-half fixes) | power: NativeGopDisplay's CPU copy costs ~39 ms per copy (½ CPU core for a blinking cursor); zero-copy scanout + idle refresh 60 Hz are the biggest display-power levers; Night Light; prerequisite of B8 | B1a survey | **B1a** read-only DPU/DSI survey kext (SSPP source address/format/stride, CTL/LM/INTF timing, PCC block, the UEFI-left mode) + `docs/drivers/dpu.md` design: zero-copy flips (SSPP address switch, SMMU stream for the DPU), refresh switching, PCC; then **B1b** zero-copy, **B1c** refresh, **B1d** Night Light | survey unattended; switching attended |
| B2 | **Cover / lid / keyboard policies** (v0.3.0 wake sources; v1.1.0 cover-close on AC locks only; v1.1.1 no instant keyboard wake after screen-on): lid open wakes (GPIO80 → SPI 633, opening only, K:b96cd03f088b); keyboard wake from screen-off and suspend via status GPIO95 → SPI 639 rising, only while the screen is off and the lid open (K:f15d206ba17e, K:afe204252af2), never GPIO97 (K:6824e7cdc219); on battery cover close sleeps, on AC it only locks; 5 min idle sleep on battery | daily use | s2idle (in flight), hall polarity verified, PDC routing (`docs/power-irq.md`) | policy spec in `docs/power.md` (suspend agent) + a small policy component (kernel side: lid/keyboard wake arming; user side: pmset defaults) | attended (lid, keys) |
| B3 | **SSC sensors**: ambient light (auto-brightness; v1.1.1 fixed readings stopping after off/on), proximity, accelerometer + gyro (auto-rotation; rotation locked while the keyboard is attached), via SSC on the ADSP (Ubuntu: hexagonrpcd + libssc + iio-sensor-proxy); the DSP's sensor framework needs its files served (registry, calibration) or it crash-loops and restarts the whole DSP (issue #3 in device.md) | auto-brightness, rotation, proximity; **also charging stability** (a crash-looping SSC restarts the charger firmware) | PianoPower level 2 healthy; QRTR/QMI (PianoPower has a codec) | `docs/drivers/sensors.md` design: the FastRPC/remote-file service the DSP's sensor framework expects, QMI SNS client over PianoPower's QRTR, sensor list, per-unit registry/calibration from `persist` (read only, never committed), the macOS side (ALS → backlight curve in PianoBacklight; accel → a rotation daemon or display transform; keyboard-attached lock) | attended later |
| B4 | **Charging policies**: PPS fast charge with the original adapter, current by charge level and temperature (v1.1.1); 9 V PD for other PD chargers, QC at 5 V; lower termination voltage from 48 °C (v0.10.0); Preserve Battery Health stop at 80 %, resume at 75 %, staying awake while charging below the limit (v1.1.0); charging restored after suspend (v0.3.0; port state could wedge after resume until re-sent, issue #18); dock charging (v0.5.0) | battery health and fast charge | PianoPower level 2 input fixed (image I) | extend `charging.md`: the policy state machine (limits, JEITA/termination, PPS/9 V, resume re-send), host tests; implementation after image I passes | attended (chargers) |
| B5 | **Power modes → macOS Low Power Mode**: Ubuntu's Power Saver/Balanced/Performance (TuneD; Balanced default) | let macOS's Low Power Mode (pmset lowpowermode, battery menu) select PianoThermal's caps and the GPU ceiling | PianoThermal `piano-cpufreq=3`, GPU idle/ceiling | find the kernel-visible signal of Low Power Mode (IOPMrootDomain property / power-source keys) and map it; design note in `thermal.md` | unattended test |
| B6 | **PMIC ADC thermistors** (11 board readings incl. battery/charger/connector, v0.7.0) | charging safety, thermal policy, menu-bar battery temperature | an ADC5-gen3 conversion is a PMIC write (opt-in) | add to PianoThermal design: channel map from the DT, conversion protocol, opt-in `piano-thermal-adc=1` | unattended test |
| B7 | **USB host / OTG at 5 Gbit/s**, Type-C role switch, both orientations (DIO4485 mux), docks with pass-through charging (v0.5.0, v0.6.0; OTG re-plug quirk) | USB sticks, keyboards, mice, docks | xHCI driver for a platform (non-PCI) controller in our KC; role/orientation from the PMIC Type-C block or the DSP's UCSI (owner 0x800b) | `docs/drivers/usb-host.md` design: which xHCI class in the KC can attach to a platform nub, DWC3 host-mode bring-up, role switching with the device-mode console, orientation/mux, VBUS (the VBUS-override write must never happen with clocks off: K:7dffd78b342d) | attended |
| B8 | **USB-C DisplayPort Alt Mode** (v0.6.0: both orientations, hot-plug, 4K 60 extended desktop; v1.1.0 USB recovery after unplug) | external displays | B1 (DPU), B7 (Type-C), DP controller + PHY, the DSP's altmode notifications (`pmic_glink_altmode`, pan_ack) | design after B1a/B7 | attended |
| B9 | **Bluetooth** on WCN7860 (`hci_qca` over UART: pairing, file transfer, audio) | keyboards/mice/headphones | PianoWifi power-up (shared chip power), GENI UART, firmware from `modem_a`/`bdwlan` at run time; macOS Bluetooth transport (IOBluetooth host-controller transport is private) | `docs/drivers/bluetooth.md` design, evaluate the macOS side first | attended |
| B10 | **Iris hardware video** decode H.264/HEVC/VP9 (AV1 software), encode H.264/H.265 1080p/4K (v0.3.0, v0.6.0, v0.10.0) | battery (video playback in software is costly), performance | Iris firmware via PAS (like the GPU ZAP/ADSP), HFI; macOS VideoToolbox side (the VM path decoded H.264 through a VT paravirt decoder: reuse its frontend) | `docs/drivers/video.md` design: native Iris + the VM-era VT frontend | attended |
| B11 | **Cameras**: rear S5KJN1SQ03 (fixed focus), front OV32D40 (v0.3.0, v0.10.0 AE fixes) | camera apps | CAMSS/CCI/CSIPHY + sensor drivers, ISP path, CoreMediaIO device | design later | attended |
| B12 | **Rear torch** (PMIC flash LED; partial in Ubuntu, v1.1.1 torch control with readback check) | flashlight | SPMI writes (opt-in), a userland trigger | small kext or PianoPower-adjacent; low priority | attended |
| B13 | **Per-unit sensor calibration** from this tablet's `persist` registry (Ubuntu's default calibration is less accurate) | accuracy of B3 | B3 | read-only reader of the registry/calibration files from `persist` at run time (or the owner's private copy under the ignored `out/`); never commit; never another unit's copy | — |

Not in scope (Ubuntu does not support them either): stylus, fingerprint, hibernation, hardware clock writes.

## 3. Order (does not preempt the power work)

1. **Now (power, in flight)**: GPU idle, CPU idle + s2idle, polling→interrupts (merged: adaptive defaults), CPU DVFS + thermal
   (merged), backlight + display sleep (merged), charging image I, Data-store guard.
2. **Power-adjacent next**: **B1a** DPU survey + design (zero-copy and 60/120 Hz are display-power levers) → **B2** lid/keyboard
   policies with s2idle → **B4** charging policies after image I → **B5** Low Power Mode mapping → **B6** thermistors.
3. **Daily-use features**: **B3** SSC sensors (also stabilises the shared DSP) + **B13** calibration → **B7** USB host/OTG →
   **B10** video decode → **B9** Bluetooth.
4. **Later**: **B8** DisplayPort Alt Mode (after B1, B7), **B11** cameras, **B12** torch; audio levels 2+ in parallel with tier 3.

Offline design started 2026-11-11: B1a (DPU survey + design), B3 (sensors design), B7 (USB host/Type-C design). Each item's page
lives in `docs/drivers/`; this table is updated when an item moves.
