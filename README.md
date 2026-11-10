# UniXNU-piano

Native macOS at EL1 on the Xiaomi Pad 8 Pro (`piano`, SM8750), as a [UniXNU](https://github.com/UniXNU/UniXNU) device
repository (same convention as UniXNU-sheng; SoC layer `UniXNU-sm8750-common`). Gunyah stays at EL2; XNU runs as the
HLOS at EL1. UniXNU core, UniXNU-sm8750-common, Mu-Silicium, piano-uefi's `pianoPkg`, DarwinBoot and the driver
components are pinned dependencies (`unixnu.lock`), not forks. Formerly the working name `xiaomipad-8pro-macos`.

Local repository only (no remote). Nothing here is flashed: the boot images are RAM images for `fastboot boot`, built and
verified offline until the USB-C cable and the rollback set are ready (`docs/el1-native-plan.md` §5).

| Path | What |
| --- | --- |
| `board.toml`, `feeds.toml`, `unixnu.lock` | UniXNU device description (schema 2, macos-27.3 line) and every pin |
| `dts/piano.dts` | piano hardware for the ADT (13 GiB DRAM window, 8 CPUs E/P, splash, reserved regions) |
| `configs/piano_macos_defconfig` | UniXNU kernel config of the 27.3 line (not built yet) |
| `configs/k27-adt.toml` | ADT identity of the k27 kernel collection (VirtualMac2,1) used by slice 1 |
| `firmware/patches/` | deltas to piano-uefi's `pianoPkg` (applied to a worktree by `tooling/firmware/setup-mu.sh`) |
| `patches/xnu/` | device patch series of the 27.3 line (empty; the k27 native variant is macOS-DroidVM 0053/0054) |
| `tooling/` | `unixnu.sh` (CLI on this device with the pinned checkouts), `build-slice1.sh` (everything), `firmware/`, `kc/`, `adt/`, `bootimg/` |
| `docs/` | plan, `el1-hyp-notes.md`, driver design pages (`drivers/`), upstream notes, evidence (`hyp-evidence/`) |
| `logs/` | committed verification logs (ADT diff, image verification, FD notes, SHA256SUMS) |
| `artifacts/ARTIFACTS.md` | SHA-256 of every built artifact and input |
| `private/` | ignored: signing key, calibration, seeds (never committed) |
| `out/`, `build/` | ignored build outputs |

## Build (slice 1)

```sh
tooling/build-slice1.sh          # SIGN=1 also AVB-signs on the private build host (the key stays there)
```

Outputs under `out/`: `fd/piano-uefi-{A-bundled,B-own303}.fd`, `k27/kc-k27-native.bin`, `adt/piano-k27.adt`,
`ram/boot-piano-{uefi,macos}-ram-{A-bundled,B-own303}[-signed].img`. Verification: `logs/bootimg/verify-*.log`.
