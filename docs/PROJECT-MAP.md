# Project map — Xiaomi Pad 8 Pro (piano)

## 中文摘要

三个公开仓库都在 GitHub 账号 [lenzhang](https://github.com/lenzhang) 下，全部公开。

- [`UniXNU-piano`](https://github.com/lenzhang/UniXNU-xiaomipad8pro) 是小米平板 8 Pro（代号 piano，SM8750）的**设备仓库**，也是当前主线：在 EL1 上原生启动 macOS。只做 `fastboot boot` **内存启动**，不刷写任何分区。
- [`UniXNU-sm8750-common`](https://github.com/lenzhang/UniXNU-sm8750-common) 是骁龙 8 至尊版（SM8750）的**公共层占位仓库**。芯片通用代码暂时仍在 `UniXNU-piano` 里，稳定后再拆过来。注意：当前构建通过 `UniXNU-piano/feeds.toml` 钉住的是上游 [`IssacL04/UniXNU-sm8750-common`](https://github.com/IssacL04/UniXNU-sm8750-common)（OnePlus 13T 谱系）。这两个仓库不要混用，在拆分完成前不要改这个 pin。
- [`veil`](https://github.com/lenzhang/veil) 存放已于 2026-11-10 冻结的精简 Linux + Gunyah 客户机路线的**公开归档**。完整的本地归档仍在本机 VM 时期工作目录的 `archive/gunyah-guest` 分支。主线请看 `UniXNU-piano`。
- Ubuntu 的修复继续提交到上游 [`yzddmr6/xiaomipad-8pro-mainline`](https://github.com/yzddmr6/xiaomipad-8pro-mainline)（内核 [`yzddmr6/linux-sm8750-piano`](https://github.com/yzddmr6/linux-sm8750-piano)），不另建仓库。
- 权威备份在 NAS 上（具体路径只记在本机，不进公开仓库）。其他机器上的副本不是权威副本。
- 密码不要写进文件、命令行参数或日志；需要 sudo 时在 tmux 里手工输入。厂商固件、分区镜像、Apple 内核 / KC / IPSW 派生产物不得进入公开仓库。
- 原生启动**只加载本仓库的 kext**：原生内核集合只链接本仓库 `package/` 里的驱动，VM 时期的半虚拟化 kext（显示、GPU、网卡、音频、输入、virtio 块设备等）一个也不链接，构建时由 `check-native-kexts.py` 强制检查（每次构建前先自检）。2026-11-11 的实测证据见 `docs/bringup-debugging.md` 第 5 节。

---

## 1. Repositories and roles

| Repository | Role | Status |
| --- | --- | --- |
| [lenzhang/UniXNU-xiaomipad8pro](https://github.com/lenzhang/UniXNU-xiaomipad8pro) | UniXNU **device** repository for the Xiaomi Pad 8 Pro (`piano`, SM8750). Panel, touch, charging, keys, DTS/ADT, boot-image packaging, RAM-boot scripts, and device docs. Current **main line**: native macOS at EL1 (Gunyah remains at EL2). | Active |
| [lenzhang/UniXNU-sm8750-common](https://github.com/lenzhang/UniXNU-sm8750-common) | SM8750 **SoC common layer** (CPU topology, GIC/AIC, memory map, UEFI SoC drivers, XNU SoC patches), following the UniXNU-sm8550-common / UniXNU-sheng split. | Placeholder (README + layout only). Code still lives in `UniXNU-piano` until the split is stable. |
| [lenzhang/veil](https://github.com/lenzhang/veil) | Thin Linux + Gunyah host / guest path (crosvm VM, macOS 27 guest on the Ubuntu host). | **FROZEN / ARCHIVED** as of 2026-11-10. Not the main line. |

Ubuntu work is **not** one of these three repositories:

- Distro / integration: upstream [yzddmr6/xiaomipad-8pro-mainline](https://github.com/yzddmr6/xiaomipad-8pro-mainline) (a public fork also exists at [lenzhang/xiaomipad-8pro-mainline](https://github.com/lenzhang/xiaomipad-8pro-mainline); send fixes upstream).
- Kernel: [yzddmr6/linux-sm8750-piano](https://github.com/yzddmr6/linux-sm8750-piano).

Do not create additional GitHub repositories for Ubuntu or for local trees such as `unixnu-research/`, `CRITICAL-BACKUP/`, `v1.1.0/`, or `rollback-os3.0.303/`.

### What goes where (device vs SoC vs host)

| Kind of change | Goes in |
| --- | --- |
| piano panel, touch, charger, buttons, board identity, `dts/piano.dts`, ADT, `board.toml`, RAM-boot / packaging scripts, device docs and logs | `UniXNU-piano` |
| SM8750 CPU topology, GIC, memory map, shared UEFI/XNU SoC drivers and SoC patches | Today: still in `UniXNU-piano`. Later: `UniXNU-sm8750-common`, then pin `feeds.toml` at the new repo |
| Frozen Gunyah / crosvm / thin-Linux guest path | `veil` (public curated archive) and the `archive/gunyah-guest` branch of the local VM-era work tree (full private copy) |
| Ubuntu userland, flashing docs, mainline kernel fixes | Upstream `yzddmr6/xiaomipad-8pro-mainline` and `yzddmr6/linux-sm8750-piano` |

**Do not confuse the two SM8750-common repositories.** `UniXNU-piano/feeds.toml` and `unixnu.lock` currently pin [IssacL04/UniXNU-sm8750-common](https://github.com/IssacL04/UniXNU-sm8750-common) (commit recorded in the lockfile) as the SoC module used to *build* piano, originally extracted for the OnePlus 13T. `lenzhang/UniXNU-sm8750-common` is our future extracted common layer. Leave the pin alone until a real split lands.

## 2. Upstream and pinned dependencies

These are dependencies, not forks we publish. Pins live in `UniXNU-piano/unixnu.lock` and `feeds.toml`. Device- or SoC-specific deltas belong in *our* trees (`firmware/patches/`, `patches/`, `dts/`, `configs/`), not in random forks of upstream.

| Project | Upstream | Notes |
| --- | --- | --- |
| UniXNU core | [UniXNU/UniXNU](https://github.com/UniXNU/UniXNU) | CLI, firmware/payload/bootimg, UniXNUPkg, macos-27.3 profile |
| SoC module currently used to build | [IssacL04/UniXNU-sm8750-common](https://github.com/IssacL04/UniXNU-sm8750-common) | Temporary pin; see above |
| SM8550 reference split | [UniXNU-sm8550-common](https://github.com/UniXNU/UniXNU-sm8550-common) + device `UniXNU-sheng` | Model for the future SM8750 split |
| UEFI / BootShim | [UniXNU/uefi-sheng](https://github.com/UniXNU/uefi-sheng) | BootShim.S |
| UEFI platform | [Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) | pianoPkg is composed on top |
| piano UEFI package | [xiaomi-pad-8-pro-development/android_device_xiaomi_piano-uefi](https://github.com/xiaomi-pad-8-pro-development/android_device_xiaomi_piano-uefi) (branch `unixnu`) | `pianoPkg`; our deltas in `UniXNU-piano/firmware/patches/` |
| DarwinBoot | [UniXNU/DarwinBoot](https://github.com/UniXNU/DarwinBoot) | XNU loader in the FD |
| Ubuntu / mainline | [yzddmr6/xiaomipad-8pro-mainline](https://github.com/yzddmr6/xiaomipad-8pro-mainline), [yzddmr6/linux-sm8750-piano](https://github.com/yzddmr6/linux-sm8750-piano) | Send Ubuntu fixes here |

## 3. Local directory layout (the owner's Mac)

Working copy (copies on other machines are not authoritative):

```
~/Downloads/XiaomiPad8Pro-Ubuntu/
  UniXNU-piano/                 # device repo (this tree). Push only a secret/blob-audited history to lenzhang/UniXNU-xiaomipad8pro
  UniXNU-sm8750-common/         # local checkout of the placeholder SoC repo
  veil/                         # local checkout of the curated public Gunyah archive
  <VM-era work tree>/           # the k27 tree (PIANO_K27_TREE) and the frozen Gunyah / thin-Linux work; branch archive/gunyah-guest
  xiaomipad-8pro-mainline/      # local clone of the Ubuntu upstream (fixes go upstream, not into a new repo)
  unixnu-research/              # pinned upstream checkouts (UniXNU, Mu-Silicium, piano-uefi, DarwinBoot, …); not published as a repo
  v1.1.0/                       # Ubuntu v1.1.0 release materials
  rollback-os3.0.303/           # official Fastboot rollback pack (never flash_all_lock)
  CRITICAL-BACKUP/              # backup notes; partition images stay out of git
  README.zh-CN.md
```

Build outputs (`UniXNU-piano/out/`, `build/`), signing keys (`private/`), vendor firmware blobs, partition dumps, and Apple kernel collections / IPSW-derived payloads stay on disk (and on the NAS) and are gitignored.

## 4. Backups

- **Authoritative copy:** a directory on the owner's NAS (its path is kept locally, not in public repositories).
- **Other machines are not authoritative.** They may hold leftover copies (for example the full official 303 package until that file is finished copying to the NAS). Do not treat their paths as the source of truth.
- NAS layout (high level): `ubuntu-v1.1.0/`, `rollback-os3.0.303/`, `partition-backup-*`, `ram-boot-ready/`, `repos/` (git bundles / trees), `docs/` (including this map).
- This document is also copied to `docs/PROJECT-MAP.md` on the NAS.

## 5. Commit and push conventions

- Commit device work in `UniXNU-piano` (the local tree on the owner's Mac). Prefer small, indicative subject lines in English (what changed, not “WIP”).
- The GitHub `main` branch of `lenzhang/UniXNU-xiaomipad8pro` is a **cleaned export**: home-LAN addresses and machine-local author emails were removed from history before the first public push. The original local `main` was not rewritten. Do not force-push the uncleaned local history to GitHub.
- Before every push to a public repository, audit the outgoing commits (worktree and history) for: passwords, tokens, keys, `private/`, SSH/NAS credentials, home-LAN IPs and hostnames, vendor firmware, partition dumps, Apple KC / IPSW payloads, and large `out/` artifacts. If a secret or blob is already in history, publish a cleaned export; never rewrite the original local repository.
- Push only to the three repositories listed above (plus Ubuntu **upstream**). Do not publish `unixnu-research/`, backups, or rollback packs.
- SoC-common code is not split yet. When it is, move it to `lenzhang/UniXNU-sm8750-common` in a dedicated change and then update `feeds.toml` / `unixnu.lock`.
- `veil` is frozen. Do not add new Gunyah-guest features there; only documentation or archive fixes, and only after the same audit.

## 6. Hard rules

1. **RAM boot only.** Use `fastboot boot`. Never `fastboot flash`, `erase`, `set_active`, `oem`, or any other command that writes a partition. Never run `flash_all_lock`.
   One exception, approved by the owner on 2026-11-11: the persistent install of [`docs/persistent-boot.md`](persistent-boot.md), which
   writes LUN 0's GPT, the end of `linux_rootfs` and `boot_a` by `dd` from the maintenance RAM image, after verified NAS backups of
   every region; never `xbl`, `abl`, `tz`, `hyp`, `vbmeta`, `dtbo`, `persist` or any `_b` partition.
2. **Never boot or flash `-signed` images** as a way to persist a new OS. Signed RAM images, if built, are a verification artifact only.
   (Exception: the signed `boot_a` image of `docs/persistent-boot.md`, under rule 1's exception.)
3. **Never `systemctl isolate`** on the tablet. It has stopped `piano-charging` and `piano-thp` and drained the battery / killed the touchscreen.
4. **No secrets in files, argv, or logs.** The tablet sudo password is typed by the user in tmux (or read from a mode-600 file *outside* every repository). Do not write passwords into scripts, commit messages, or `tmux` send-keys.
5. **No vendor blobs or Apple payloads in public repos:** Xiaomi / Qualcomm firmware, partition dumps, IPSW-derived kernelcaches, trust caches, `.img` / `.fd` / `.kc` / `.ipsw` build outputs. Record SHA-256 in `artifacts/ARTIFACTS.md` instead.
6. **No home-LAN inventory in public repos:** private LAN addresses, the owner's machine and NAS hostnames, NAS backup paths, and machine-local author emails. Local working copies may keep operational defaults; public history must not.
7. Push only to the repositories named in this document (and Ubuntu upstream).
8. **Only this repository's kexts in a native boot (the native kext rule).** The native kernel collection links only this
   repository's kexts (descriptors in `tooling/k27/kext/`, sources in `package/`, objects in `out/native-kexts/`, the set in
   `configs/k27-native-kexts.txt`). None of the VM-era paravirtual kexts (display, GPU, Ethernet, audio, video, input, virtio
   block) is linked, not even for one test. VM-era code that native piano needs is taken over as this repository's own package
   (with `UPSTREAM`), never by linking the VM-era kext. `tooling/kc/build-kc-k27.sh` enforces this
   (`tooling/k27/check-native-kexts.py`: a self-test of its denylist, then the descriptors before the link and the collection
   after it), and `tooling/persist/build-esp.py` refuses an ESP whose collection has a VM-era bundle. Evidence, the boot
   arguments still read under legacy names, and the ADT identity: `docs/bringup-debugging.md` §5.

## 7. Current bring-up note (2026-11-11 11:30)

Native macOS is installed on piano and boots by itself: boot_a = BootShim + FD (signed v4), the UniXNU payload on an ESP (LUN 0
partition 37, carved from linux_rootfs), the macOS root and Data on linux_rootfs (NativeUfs). The A830 GPU (PianoA830 + Turnip on
ABI v2) composites the login screen. Dev work continues with RAM boots (`fastboot boot`, which take priority over the installed
copy); verified backups of every written region are on the NAS and `tooling/persist/maint-session.py rollback ...` restores them.
See `docs/persistent-boot.md`. The Data store (its format, the store guard that keeps the installed store from RAM images with
another seed, flatten, the test procedure): `docs/data-store.md`.

Drivers and their state (each inert without its boot argument; pages in `docs/drivers/`):

| Driver | State |
|---|---|
| PianoDwc3 (USB console, monitor incl. `reset fastboot`, the Mac's keyboard/pointer) | runs on the tablet |
| PianoPower (gauges, Type-C; DSP charger path) | level 1 runs; level 2 starts the DSP but the input stays at 0 mA (diagnostic images G/H pending) |
| PianoKeyboard (WN8030 cover + touchpad, hall gpio80) | MCU boots and the cover attaches; key/touchpad reports not yet seen with someone typing |
| PianoTouch (NT36532E on QUP1 SE2) | built and linked, not run |
| PianoWifi (WCN7860 on PCIe0) | level 1 read-only survey built, not run; the full ladder is months of work (`wifi.md`) |
| PianoBacklight (2x KTZ8866: boot level, off on display sleep, key steps) | built, host-tested and linked, not run (`backlight.md`) |
| PianoInput (buttons) | runs on the tablet |

The installed profile (`build-esp.py --profile daily`: GPU + PianoPower level 1 + the USB console and the Mac's keyboard/pointer)
replaces the first install's GPU-only profile at the next install session.
