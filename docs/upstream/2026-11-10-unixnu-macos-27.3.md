# UniXNU upstream at the macOS 27.3 milestone (pulled 2026-11-10 ~18:35 SGT)

Checkouts under `~/Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research/`, fast-forwarded to upstream.

## What changed for 27.3 (relevant to piano)

- **UniXNU core** `UniXNU/UniXNU` 5dc00d2 → **d830d23** (51 new commits, all dated 2026-11-10; upstream history was
  rewritten, so the old local tip is kept as branch `old/main-5dc00d2`).
  - `profiles/os/macos-27.3-26C5035r/`: the first macOS OS profile. Pins `UniversalMac_27.3_26C5035r_Restore.ipsw`,
    identity **Mac17,11 / K114cAP** (T6050, `chip_id` 0x6050 in sheng's override), the symbol-bearing `mac17j`
    kernelcache (`d1003b51…`), a macOS fileset root list (USB host + device stacks, HID, IOGPU, IOMFB, APFS,
    DiskImages, IO80211, IOBluetooth, AppleTopCase…) and `early_security_init` for AMFI.
  - Kernel: still **xnu-12377.121.6** (ac9718f, same base as our k27), built for the iOS platform with
    `target = "macosx"` (defines `XNU_TARGET_OS_OSX`: padded vtables, macOS paths) plus a 26-patch macOS series:
    Darwin 26 syscalls/KPIs, 8 GiB arm64 shared region, 47-bit user VA, union one-volume root, owner Data firmlinks,
    read-only system volume + apfs boot shim, kqworkloop limits, exec-log, verbose text console (`vc-text=1`),
    app cryptex (Safari), swap files, and **`macos-droidvm-k27-parity.patch`**. README: the macOS 27 profile
    follows the earlier VM-based port for its kernel patches (union root, 4K code-signature pages, 24 MHz timebase, realtime
    and mmap fixes, exec denial) and for the account setup in the Data seed. The k27 baseline series' changes are now
    upstream in spirit.
  - Payload: macOS Data seed with accounts/auto-login/home, GPU bundle layout + prime list, addon `/usr/sbin` and
    `/usr/lib` overlays, `/chosen/asmb` local boot policy, boot volume named in `/chosen`.
  - Boot args lost the `sheng-` prefix (`dpu-scanout=2`, `fb-bgra=1`, `touch=4`, …).
- **UniXNU-sheng** (Pad 6S Pro, SM8550, the 27.3 + touch milestone board) 6894001 → **00ba909** (+60):
  `board.toml` gains `os_overrides."macos-27.3-26C5035r"` (kernel archives, components, payload 200 MiB, a long
  macOS kernel-arg set incl. `timebase-hz=24000000`, `dpu-scanout=2`, `fb-bgra=1`, `touch=4 touch-hid=1`,
  `keyboard=1`, `wifi=5`, `ethernet=1` (Wi-Fi carried as en0), `audio=12 adsp=2`, `gpu-metal=1`), macOS
  defconfig `sheng_macos_defconfig`, the keyboard cover (WN8030) as keyboard + pointer on macOS, Bluetooth runtime for
  macOS, power: GPU floor between light frames, touch controller back to doze. **This board is the template for
  the piano macOS layer.**
- Components moved on 2026-11-10: DarwinBoot 8544957 (reads kernels up to the default window size; loads the macOS
  KC), GopFramebuffer d711e8c (builds for macOS, display attributes/UUID, console release on macOS),
  ShengUfs b11de87 (8 concurrent commands, Linux-ufshcd-like queueing), ArmDTPlatform e8bd8d5 (board clock,
  calendar), ArmCpu 1604083, XnuPrelink 8f4f386, AdrenoMetal c68f9a1, QcaBluetooth b2fbab6, Ath12kWifi 48e23bf.
- **Unchanged:** `UniXNU-sm8750-common` feaa135 (2026-11-04, OnePlus 13T peripherals), `UniXNU-Pagani_SM8750`
  25b1109, `Adreno830` 1a13210, `hackios-mu-silicium` 90721af, `uefi-sheng` 66421cb, `ShengNovatekTouch` 69fdd62.
  There is no macOS profile on any SM8750 board yet; piano would be the first.

## Piano-specific upstream found during the pull (new to this workspace)

| Repo | HEAD | What |
| --- | --- | --- |
| `xiaomi-pad-8-pro-development/android_device_xiaomi_piano-unixnu` (`piano-unixnu/`) | 377ad53 (11-05) | UniXNU device repo for piano, iPadOS 17.7.11, SM8750 via UniXNU-sm8750-common + IssacL04 Pagani drivers; "not booted yet" |
| `xiaomi-pad-8-pro-development/android_device_xiaomi_piano-uefi` branch `unixnu` (`piano-uefi/`) | 31545f9 (11-05) | Mu `pianoPkg` with "UniXNU modules behind UNIXNU", selector + UEFI in boot.img, worked on HyperOS 4.0.0.44 |
| `code002-2/Unixnu-SM8750-Piano` (`Unixnu-SM8750-Piano/`; renamed from `UniXNU-piano/` so that name means only this repository) | c2d3e13 (11-08) | CI-only macOS build host for the above; ships `inputs/piano-live.dtb` |
| `slwyts/Project-SunUEFI` (`Project-SunUEFI/`) | 4ef8740 (11-10) | the most active piano UEFI: combined boot (Android/UEFI selector), `PianoUEFI-product.img`, a read-only UEFI fastboot service, Linux desktops boot from it |

## Consequence for the plan

- Native kernel line = UniXNU core + `macos-27.3-26C5035r` + `UniXNU-sm8750-common` + this repo's piano layer, in the
  shape of sheng's macOS override. k27 (27.0.1, our 52 patches) stays the VM reference and the VM test rig (item 4).
- `pianoPkg` is not written from `annibalePkg`: an existing piano Mu platform (piano-uefi `unixnu` or SunUEFI) is
  pinned and only the deltas live here.
