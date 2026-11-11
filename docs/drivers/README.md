# Native Darwin drivers for piano: design pages

Plan item 5 of the first implementation slice (`../el1-native-plan.md` §8, driver list in §4(c).4 and §4(d)).
One page per driver, written before any code. Status of every page: **design, 2026-11-10, nothing built or run on
hardware.** Hardware tests start after M2 (XNU RAM boot) or M3 (UFS), always with the USB-C cable and the owner present.

## Index (order = what gates a login, plan §4(d))

| # | Page | Driver(s) | Gate | UniXNU base |
|---|---|---|---|---|
| 1 | [display.md](display.md) | IOMFB top half (from DroidVMDisplay) + GOP/splash bottom half, then DPU address-only flips, backlight | M2: first frame and the login window on the panel | `GopFramebuffer`, `dpu-scanout=2` pipe adoption (`docs/porting/display.md`), UniXNU-sheng backlight |
| 1a | [native-gop-display.md](native-gop-display.md) | `NativeGopDisplay`: display.md's GOP step as a k27 native kext (DroidVMDisplay's CPU copy), `/arm-io/gop-display` | M2a: WindowServer's frames on the panel (built and linked 2026-11-11; Metal is the open blocker) | macOS-DroidVM `DroidVMDisplay` (GopFramebuffer read) |
| 1b | [soft-metal.md](soft-metal.md) | `NativeSoftGpu` + `NativeSoftMetal`: a Metal device with no GPU, so SkyLight and Core Animation composite in software | M2a without a GPU driver: WindowServer stays up and submits swaps (2026-11-11: it takes the device, then aborts; v2 answers every device selector and reports the abort) | macOS-DroidVM `DroidVMGpuKext`, `NativeAdrenoMetalPlugin` |
| 1c | [backlight.md](backlight.md) | `PianoBacklight`: the panel backlight (2x KTZ8866 over software I2C on their QUP pins): a moderate boot level, off on display sleep, on at wake, brightness steps (the Mac's keys; the cover's via its driver), optionally off with the cover closed | daily use on battery (built and host-tested 2026-11-11, in the kext list, inert without `piano-backlight`; not run) | UniXNU-sheng `ShengBacklight` (design), PianoKeyboard `BitI2c.h` |
| 2 | [ufs.md](ufs.md) | `NativeUfs` for `qcom,sm8750-ufshc` | M3: real root/Data instead of the RAM seed | `ShengUfs` (13T host-verified) |
| 3 | [touch.md](touch.md) | `PianoTouch`: NT36532E over GENI SPI (SE DMA, or SPI by hand), the host download, a single-touch tablet pointer | login by touch (keyboard cover is the fallback) (built and linked 2026-11-11, inert without `piano-touch`) | Project-SunUEFI's piano touch producer + `QcomGeniSpi` (NativeGeniSpi) |
| 3a | [keyboard.md](keyboard.md) | `PianoKeyboard`: the keyboard cover (Nanosic WN8030 over software I2C) as keyboard + pointer, the folio hall sensor (read only) | login typed on the tablet (level 1 ran 2026-11-11: MCU booted, cover attached; no key/touch report seen yet) | UniXNU-sheng `Wn8030` + `KeyboardCover` |
| 4 | [charging.md](charging.md) | `PianoPower`: fuel gauges over software I2C, the charger PMIC's port (SPMI, read only), the DSP's charger firmware (PAS + GLINK + MCA properties) | any native session longer than the battery; macOS as the default OS (built and linked 2026-11-11, inert without `piano-power`) | UniXNU-sheng ADSP/GLINK/QRTR transport, Adreno830 Sm8750Scm, Project-SunUEFI PMIH010x facts |
| 5 | [gpu.md](gpu.md) | Adreno 830 KMD + Metal glue | GPU-composited desktop (M5) | `Adreno830` + `Sm8750Platform` + `AdrenoMetal` (13T Metal desktop) |
| 5a | [gpu-userspace.md](gpu-userspace.md) | the VM's `NativeAdrenoMetal.bundle` with Turnip on Adreno830's ABI v2 (`tooling/metal`) | the userspace of M5 (built and checked offline 2026-11-11, staged when PianoA830 is linked) | macOS-DroidVM's plugin/bridge, Droid-VM Mesa `cecc0e9` |
| 5 | [gpu.md](gpu.md) | `PianoA830`: the Adreno830 A830 KMD as a k27 native kext + piano's board glue, AGXAccelerator for the Metal plugin | GPU-composited desktop (M5) (built and linked 2026-11-11, inert without `piano-gpu`; the read-only survey ran on the tablet, the S1..S10 ladder is being walked) | `Adreno830` + `AdrenoMetal` (13T Metal desktop) |
| 6 | [wifi.md](wifi.md) | `PianoWifi`: the WCN7860 (17cb:110e) on PCIe0; level 1 (read-only survey) built and linked, inert without `piano-wifi`; the ladder to MHI, QMI, WMI, scan and join; macOS sees an Ethernet-like interface | M5: a real network (a USB-C network first) | UniXNU-sheng `WifiProbe`, PaganiWifi (SM8750, same chip), Ath12kWifi (GPL sources: owner decision) |
| 7 | [audio.md](audio.md) | `PianoAudio`: four FS19xx amplifiers on the LPAIF secondary TDM and the VA-macro DMICs, through the ADSP's AudioReach (GPR on `adsp_apps`); level 1 (read-only survey) built and linked, inert without `piano-audio`; the ladder to the DSP link, PRM clocks, amplifiers, graphs, a tone, a macOS device (IOAudio2, as the VM's), volume, microphones | none (after M5) | UniXNU-sheng GPR/AudioReach (SM8550, hardware-verified), PianoPower's DSP, macOS-DroidVM `DroidVMAudio` (the macOS device) |
| 8 | [sensors.md](sensors.md) | `PianoSensors`: the ADSP sensor hub's ambient light (automatic brightness into PianoBacklight) and accelerometer (display rotation, normal while the cover is attached) on PianoPower's DSP: the FastRPC reverse file service the sensor PD needs (this unit's registry, read only), a QMI client of service 400, a safety governor for the shared charger DSP; host-tested code only (2026-11-11), no kext yet | none (daily use) | UniXNU-Pagani-Drivers `PaganiSensors` (SM8750, BSD, host-tested), PianoPower's DSP and router |
| 4b | [thermal.md](thermal.md) | `PianoThermal`: the four TSENS controllers (read only), a protection policy on the stock trips (GPU park, critical alarm, opt-in halt), the CPU clusters over SCMI perf + the CPUCP doorbell (report, a fixed Power Saver level, a load-based governor with Ubuntu's caps) | daily use in a stand, battery life (built and linked 2026-11-11, not run; inert without `piano-thermal` / `piano-cpufreq`) | Adreno830 `Sm8750Tsens` facts, the K90 port's SCMI facts, UniXNU-sheng `CpuFreqProbe` design |
| 4a | [rtc.md](rtc.md) | k27 0060 (`piano-rtc=1`): the calendar from the PMK8550 RTC + an offset; PianoPower's read-only RTC readout | a wall clock that survives reboots (built and linked 2026-11-11, not run; inert without `piano-rtc`) | k27 0057's SPMI reads, PianoPower `SpmiObserver.h` |
| – | [usb-host.md](usb-host.md) | `PianoUsbHost`: USB host / OTG and the Type-C port (roles, orientation, VBUS via the DSP); the macOS-side verdict (Apple's `AppleUSBXHCI` on a hand-made nub; the host stack links natively with a larger stand-in); level 1 read-only survey built and linked, inert without `piano-usbhost` | none (daily use: sticks, keyboards, docks; backlog B7) | QcomDwc3 (SM8550: Apple's xHCI on a Qualcomm DWC3), PianoDwc3, PianoPower's DSP |
| – | [usb-device-debug.md](usb-device-debug.md) | `PianoDwc3`: DWC3 device mode, two CDC-ACM ports (live kernel console + monitor, user pipe) | none (debugging aid for all of the above) | own gadget core; takeover order from Project-SunUEFI's piano DWC3 driver (Apple's XDCI class is not in the macOS KC) |

Not covered yet (plan §4(d).6, later): Bluetooth (`QcaBluetooth`).

## Common template

Every page uses these headings, in this order. Keep a page between roughly 80 and 200 lines.

1. **Purpose and gate.** What the driver does and which login milestone (M1..M5 of plan §5) it unblocks.
2. **Hardware facts.** Register blocks, IRQs, SMMU SIDs, clocks/regulators/GDSCs, firmware files. Every fact has a
   source (DT node, dossier section, kernel file). Mark each fact *verified* (read from source/live DT), *inferred*, or
   *needs-boot*.
3. **Ubuntu as the specification.** Exact files and functions in the Ubuntu piano stack, described behaviourally
   (sequence, registers, timings). Sources:
   - kernel: `yzddmr6/linux-sm8750-piano` branch `piano/v1.1.0`, commit `d2710cd1` (Linux 7.2.15), checked out
     read-only at the private build host `~/kfirst/linux` (tag v1.1.0) and on the tablet at `~/piano-kbuild/linux`;
     pinned by `xiaomipad-8pro-mainline/kernel/source.json`;
   - userspace and packaging: `xiaomipad-8pro-mainline/` (`device/`, `kernel/`, `docs/`, `packaging/`);
   - measurements and reports: `droidvm-build/piano-patches/*.md`, `droidvm-build/PROGRESS-thin-macos.md`.
4. **UniXNU template to build on.** Which existing component is the base (do not reinvent), its pinned commit, and
   the delta for piano. Local checkouts: `unixnu-research/` (UniXNU core docs in `UniXNU/docs`, the macOS 27.3 board
   template `UniXNU-sheng`, see `../upstream/2026-11-10-unixnu-macos-27.3.md`).
5. **IOKit family / ABI.** The class the driver publishes and the family it binds to (IOMFB 27.x, IOBlockStorageDevice,
   IOHIDFamily, IOPMPowerSource, IOGPU/AGX).
6. **Boot args.** UniXNU 27.3 naming (no `sheng-` prefix), default value, what each value does.
7. **Kernel archive / component.** Name in the piano board's `[kernel] archives` / `components` (shape of UniXNU-sheng
   `os_overrides."macos-27.3-26C5035r"`), started by `Stage7Probe`.
8. **Hardware test plan.** After which milestone, what to observe, pass criteria, and how it fails safe (RAM boot,
   read-only first, Ubuntu returns on reboot).
9. **Fallback.** What runs if the driver is disabled or fails.
10. **Open questions.** Numbered, each with how it gets answered (source read, VM test, needs-boot).
11. **Licence note.**

## Rules that apply to every page

- **Licence.** Linux drivers (GPL-2.0) are read as the specification only. Pages describe behaviour and cite
  `file:function`; no Linux code is pasted into pages or into Darwin sources. Darwin code is written fresh, or derived
  from UniXNU components under their own licences (check each repo's `LICENSE` before copying). The user's own
  userspace (for example `device/piano-thp`) is checked for its SPDX header before reuse.
- **Privacy.** No passwords, keys (`boot-signing.pem`), MAC addresses, ECID, serials or calibration values in this repo.
  `thp-calibration.json` and other per-unit data are referred to by file name only and live under `private/`.
- **Safety.** No boot-chain write before M4. Every first hardware test of a driver is read-only or output-only and runs
  from a `fastboot boot` RAM image, so a reboot returns to Ubuntu. Never leave a native boot unattended (no thermal
  or charging control early, plan R4/R5).
- **Boot-arg gate.** Every driver is off unless its boot arg is set, so a bad driver is disabled by rebuilding the RAM
  image's kernel args, not by rebuilding the kernel.
