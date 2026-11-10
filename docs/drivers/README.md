# Native Darwin drivers for piano: design pages

Plan item 5 of the first implementation slice (`../el1-native-plan.md` §8, driver list in §4(c).4 and §4(d)).
One page per driver, written before any code. Status of every page: **design, 2026-11-10, nothing built or run on
hardware.** Hardware tests start after M2 (XNU RAM boot) or M3 (UFS), always with the USB-C cable and the owner present.

## Index (order = what gates a login, plan §4(d))

| # | Page | Driver(s) | Gate | UniXNU base |
|---|---|---|---|---|
| 1 | [display.md](display.md) | IOMFB top half (from DroidVMDisplay) + GOP/splash bottom half, then DPU address-only flips, backlight | M2: first frame and the login window on the panel | `GopFramebuffer`, `dpu-scanout=2` pipe adoption (`docs/porting/display.md`), UniXNU-sheng backlight |
| 2 | [ufs.md](ufs.md) | `NativeUfs` for `qcom,sm8750-ufshc` | M3: real root/Data instead of the RAM seed | `ShengUfs` (13T host-verified) |
| 3 | [touch.md](touch.md) | NT36532E over GENI SPI (+ keyboard cover note) | login by touch (keyboard cover is the fallback) | `ShengNovatekTouch` + `QcomGeniSpi` |
| 4 | [charging.md](charging.md) | power source / charger policy / gauges | any native session longer than the battery | UniXNU-sheng power records, Project-SunUEFI power work |
| 5 | [gpu.md](gpu.md) | Adreno 830 KMD + Metal glue | GPU-composited desktop (M5) | `Adreno830` + `Sm8750Platform` + `AdrenoMetal` (13T Metal desktop) |

Not covered yet (plan §4(d).6, later): Wi-Fi (`Ath12kWifi`), Bluetooth (`QcaBluetooth`), audio, sensors, keyboard
cover as its own page (touched on in `touch.md`).

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
