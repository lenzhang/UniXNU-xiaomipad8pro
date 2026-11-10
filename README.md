# xiaomipad-8pro-macos

Native macOS at EL1 on the Xiaomi Pad 8 Pro (`piano`, SM8750), built on [UniXNU](https://github.com/UniXNU/UniXNU).
This repository is the UniXNU **device layer** for piano plus the board-independent `tooling/` that came out of the
macOS-DroidVM work. UniXNU core, `UniXNU-sm8750-common`, Mu-Silicium and the driver components are pinned
dependencies (`unixnu.lock`), not forks.

Local repository only. Nothing here is flashed; RAM-boot images are built and verified offline until the USB-C cable
and the rollback set are ready (`docs/el1-native-plan.md` section 5).

| Path | What |
| --- | --- |
| `board.toml`, `porting.toml`, `feeds.toml`, `unixnu.lock` | UniXNU device description and pins |
| `dts/piano.dts` | piano hardware description, ADT source |
| `configs/`, `patches/xnu/` | device kernel config and patch series |
| `package/` | device drivers (Darwin, written fresh; Ubuntu drivers are the spec only) |
| `firmware/` | UEFI platform (`pianoPkg`) selection and pins |
| `tooling/` | k27 patch set, KC tooling, ADT generator, boot-image packaging/verification |
| `docs/` | plan, hypervisor notes, driver design pages |
| `artifacts/ARTIFACTS.md` | SHA-256 of every built artifact |
| `private/` | ignored: signing key, calibration, seeds (never committed) |
