# UFS: NativeUfs (ShengUfs lineage) for qcom,sm8750-ufshc

Status: design, 2026-11-10. Nothing built or run. Template: `README.md`.

## 1. Purpose and gate

Block storage for the macOS root (read-only, verified) and the Data volume (read-write window) on piano's UFS, so
that native sessions stop depending on the RAM seed. Gates **M3** (plan §5). Until M3 the boots use `RamBlock` + the Data
seed, and UFS is probed read-only.

Scope: adopt the link that UEFI left up (no PHY/clock cold init), single-doorbell transfer requests, LUN 0 only for
writes, named windows inside our own partitions. Out of scope: ICE/inline crypto, MCQ, runtime power management
(hibern8 entry, clock gating), writes outside the Data window.

## 2. Hardware facts

| Item | Value | Source | State |
|---|---|---|---|
| Host controller | `ufs_mem_hc: ufs@1d84000`, `qcom,sm8750-ufshc` / `qcom,ufshc` / `jedec,ufs-2.0`; `std` 0x1d84000 len 0x3000, `mcq` **0x1da0000** len 0x7000 (= std + 0x1C000) | `sm8750.dtsi:5698-5703`; `ufs-qcom.h:78` | verified |
| IRQ | GIC **SPI 265**, level-high (same number ShengUfs uses on sheng) | `sm8750.dtsi`; `ShengUfs/NativeUfs.cpp` | verified |
| SMMU | apps SMMU `0x15000000`, **SID 0x60** mask 0; `msi-parent` gic_its 0x60; node is **`dma-coherent`** | `sm8750.dtsi` ufshc node | verified |
| PHY | `ufs_mem_phy: phy@1d80000`, `qcom,sm8750-qmp-ufs-phy`; clocks ref (RPMH_CXO), ref_aux, qref (TCSR_UFS_CLKREF_EN); GDSC `GCC_UFS_MEM_PHY_GDSC` | `sm8750.dtsi:5675` | verified |
| Clocks / power | 8 clocks (core, bus_aggr, iface, unipro, ref = LN_BB_CLK3, tx0/rx0/rx1 symbol); OPP 100/403 MHz; reset `GCC_UFS_PHY_BCR`; GDSC `GCC_UFS_PHY_GDSC` (required-opp nom); interconnects ufs-ddr, cpu-ufs. GCC offsets: UFS GDSCR 0x77004, PHY GDSCR 0x9e000 | `sm8750.dtsi`; `UniXNU-sm8750-common/kits/Sm8750Platform/GccUfs.h` | verified; left on by UEFI: needs-boot |
| Lanes / gear | 2 lanes per direction; Linux forces HS-G5 Rate A on HW v5 | DT; `ufs-qcom.c:ufs_qcom_power_up_sequence` | verified |
| Piano supplies | vcc `vreg_l17b_2p5` (2.504 V), vccq `vreg_l1d_1p2`, PHY vdda `vreg_l1j_0p91`/`vreg_l3g_1p2`; reset GPIO tlmm 215 active-low; ICE deleted; QREF/REFGEN, reset-pad rail and VCCQ parent "left as firmware set them" | `sm8750-xiaomi-piano.dts:1650-1667` | verified |
| LUNs | 6 LUNs, 4096-byte logical blocks | `xiaomipad-8pro-mainline/docs/INSTALL.en.md:85` | verified |
| LUN 0 layout | ..., 34 `userdata` (Android), 35 `linux_rootfs` (60 GiB), 36 `linux_data` (~201 GiB on 512 GB units, ~61 GiB on 256 GB); LUN 0 = 124,552,192 blocks (512 GB) | `INSTALL.en.md:208-228`, `README.en.md:61` | verified |
| Crypto | `crypto@1d88000` ICE, removed on piano (`qcom,ice` deleted) | piano dts | verified |

## 3. Ubuntu as the specification

Kernel `linux-sm8750-piano` v1.1.0, `drivers/ufs/host/ufs-qcom.c`, `drivers/ufs/core/ufshcd.c`. Read, do not copy.

- **Binding:** `ufs_qcom_of_match` (:3038) has no sm8750 entry; piano binds through generic `qcom,ufshc` without
  drvdata (no `BROKEN_LSDBS_CAP`, no `no_phy_retention`). Config `piano-persistent.config:5-8` builds UFSHCD, UFS_QCOM
  and the QMP UFS PHY in; crypto and BSG off.
- **Cold path we do not repeat:** `ufs_qcom_init` (:1594: resets, PHY, ICC, reset GPIO, lane clocks, caps, ICE) and
  `ufs_qcom_hce_enable_notify` (:620: pre = `ufs_qcom_power_up_sequence` :519 — host+PHY reset, phy init/mode/power-on/
  calibrate, UniPro mode, then lane clocks; post = hibern8 check, HW clock gating via CFG2 0xE0, ICE). Native XNU
  relies on UEFI's UFSDxe having done all of this.
- **Device bring-up order (for the adoption path's sanity checks):** `ufshcd_hba_enable` (:5135) → `ufshcd_link_startup`
  (:5264, DME_LINKSTARTUP) → `ufshcd_verify_dev_init` (:5362, NOP OUT/IN) → `ufshcd_complete_dev_init` (:4967,
  set fDeviceInit, poll clear) → `ufshcd_probe_hba` (:9423, power mode change) → `ufshcd_add_lus` (:9185) and the well-known
  LUs (`ufshcd_scsi_add_wlus` :8445).
- **MCQ (Linux only):** `use_mcq_mode` defaults to true (:113); `ufs_qcom_mcq_config_resource` (:2321) maps `mcq`,
  `ufs_qcom_op_runtime_config` (:2343) places SQD/SQIS/CQD/CQIS at MCQ config + 0x5000/0x5040/0x5080/0x50C0 (stride
  0x100), `ufs_qcom_config_esi` (:2429) uses ITS MSIs. So Linux on piano most likely runs MCQ+ESI (inferred from DT and
  code; confirm from a saved dmesg). We do **not** follow it: XNU uses the legacy doorbell.
- **Vendor registers worth reading only:** HW_VERSION 0xE4, CFG0/1/2 0xD8/0xDC/0xE0, SYS1CLK_1US 0xC0, H8 counters
  0x2700-0x2710 (`ufs-qcom.h:100-157`).
- **Partitions:** found by name only (`root=PARTLABEL=linux_rootfs`, `tools/build-persistent-boot.py`).
  `docs/FLASHING.en.md` rules (never `vbmeta`, `dtbo`, `persist`, `_b`, `--set-active=b`). The RAM maintenance image
  (`initramfs/piano-maint`) mounts nothing and prints LUNs and `sgdisk -p`.

## 4. UniXNU template to build on

**Base: `ShengUfs` b11de87** (runtime class `NativeUfs`; doc `UniXNU/docs/porting/ufs.md`, `docs/drivers/sheng-ufs.md`).
It already carries the macOS 27.3 pieces piano needs (owner Data window, read-only verified root, root publication by
volume UUID, apfs boot shim) on UniXNU-sheng. **SM8750 deltas from `UniXNU-Odin3/drivers/NativeUfs` (6d58c09)**, the
SM8750 fork used on OnePlus 13T/Ace 6/Odin 3 (`NATIVE_UFS_SM8750` ifdefs; Pagani's own copy is an unchecked-out
submodule `IssacL04/UniXNU-Pagani-Drivers`).

What ShengUfs does (keep): `NativeUfs : IOService` starts only with `native-ufs-probe`/`native-ufs-root`
(:2421); `hostBringUp` (:1138) maps the HC, puts stream 0x60 in SMMU pass-through, takes over the request list, sends
START STOP UNIT (active) to well-known LU 0xD0 (:1370); reads the link with DME_GET and leaves it (HS-G5 A x2 FAST);
GCC is read only (`GccUfs.h`). Legacy single doorbell (UTRLDBR 0x58), 32 slots (`RootTransportLayout.h:16`, last slot
kept for the panic path; falls back to 1 slot without contiguous memory), max 1 MiB/command, 64-entry PRDT, 128 KiB
bounce area, completion by SPI + UTRCE with 200 us polling first. Probe mode 3 loops 6 LUNs with TUR/READ CAPACITY
and requires 4096-byte LUN 0 (:628, :663); GPT scan by name (`native_ufs_part_find` :1600). `NativeUfsBlock :
IOBlockStorageDevice` (:1800) exposes named windows, reports 512-byte blocks over 4 KiB device reads; all writes refused
(`doAsyncReadWrite` :2127) except the Data window (`data-persist=2`, :1409-1416: window at the end of the partition,
clear of the root image + 4 MiB guard, at most a quarter of it).

Changes for piano:
1. **MCQ guard** from Odin3 `RootQueue.h:hostStopQueue`: refuse takeover if CAP bit 30 and MCQ enable (0x300 bit 0) are
   set; require HCS ready, doorbells 0x58/0x78 idle, no fatal IS before clearing 0x24/0x60. If UEFI ever leaves MCQ on,
   the options are a clean switch back to SDB (open question) or MCQ support (not planned).
2. **SM8750 link handling** from the same fork: disable AHIT (0x18), hibern8 exit (UIC 0x18) and a DME_GET probe via
   0x90/0x94 before the first command.
3. **Partitions:** piano has no `unixnu` partition; the macOS windows live in partitions carved from the tail of
   `linux_data` at M3 (plan §6): `macos_os`, `macos_systemos`, `macos_data`, or one `macos` partition with
   ShengUfs-style windows. Decision: prefer **one partition with windows** (matches ShengUfs's `[userdata]`
   `first_lba/last_lba/lba_bytes` model and its verified-root guard) unless the APFS images need separate GPT entries.
   Partitioning procedure: ours (RAM maintenance image, `resize2fs` + `sgdisk`), not UniXNU's
   `scripts/dual_sys/partition.py` (F2FS userdata, TWRP).
4. **Coherency:** DT says `dma-coherent`; ShengUfs assumes non-coherent (write-combined list page, cache maintenance).
   Keep the maintenance (harmless) and note it.
5. The display SMMU window code also lives in this component (`dpuEnable`, see `display.md`); piano stream IDs there.

## 5. IOKit family / ABI

`IOBlockStorageDevice` (IOStorageFamily) → IOBlockStorageDriver → IOMedia (GPT scheme) → APFS. No IOSCSI families:
UPIU and SCSI CDBs are built in the driver. The root is matched by volume UUID (`RootPublication.h`,
`payload.adt.root_match_volume_uuid`).

## 6. Boot args

`native-ufs-probe=N` (read-only probe levels; 3 = LUN loop), `native-ufs-root=1` (publish root windows),
`native-ufs-diagnostics=1`, `native-ufs-stats-delay`, `data-persist=1` (prove the window) / `=2` (writes on),
`native-source-storage=1` (source-storage runtime), `apfs-boot-shim=1`, `log=1`. UniXNU-sheng's macOS set:
`native-source-storage=1 native-ufs-root=1 log=1 data-persist=2 exec-log=1 apfs-boot-shim=1`.

## 7. Kernel archive / component

`components`: `ShengUfs` (pinned b11de87) + a piano SM8750 patch set in this repo (or the Odin3 `NativeUfs` if it
reaches parity for macOS); `[kernel] archives`: `component:ShengUfs`, a piano `build-piano-source-storage.py` modelled
on `build-sheng-source-storage.py --policy-dir scripts/policies/macos-27.3-26C5035r`, `component:RamBlock` (kept for
the seed path). `[userdata]` block with the window LBAs after M3; no per-unit values committed (they differ between
256 GB and 512 GB units: read them from the GPT at build time).

## 8. Hardware test plan

All steps from a `fastboot boot` RAM image, cable connected, owner present.

1. **M2, `native-ufs-probe=1..2`:** map, read CAP/VER/HCS/IS, MCQ enable bit, doorbells, HW_VERSION, DME_GET of the
   gear. No command issued. Pass: HCS ready, doorbells idle, MCQ off (or a clean refusal), HS-G5 x2.
2. **M2, probe 3:** NOP/TUR/READ CAPACITY on 6 LUNs, block size 4096 on LUN 0, GPT names match `sgdisk -p` from the
   RAM maintenance image.
3. **M3a, read-only reads:** hash a known range of `linux_rootfs` (and a raw range of LUN 0) in XNU and compare with
   the same range hashed on Ubuntu. Read throughput at QD 1/8/32 vs Linux (UniXNU's ufs.md performance section is the
   reference method).
4. **M3b, partition change** (RAM maintenance image, not XNU): shrink `linux_data`, create the macOS partition(s), write
   the APFS containers built by the VM recipes, verify SHA-256; rollback = `RESTORE-20261110.md` scenario C (GPT
   backup reload per LUN, `partx -u`, diff `sgdisk -p` against `gpt.txt`, restore partitions).
5. **M3c, root from UFS:** `native-ufs-root=1`, Data still from the RAM seed. Then `data-persist=1` (window proof),
   then `=2`. Pass: a file written in macOS survives a reboot, Ubuntu's `linux_rootfs`/`linux_data` hashes unchanged.
6. **Soak:** the 13T record has "a UFS read timed out after 297989 reads" (`oneplus13t.toml:176-182`): run a long
   read soak (e.g. 1 M reads) and check timeout handling (abort + retry, not panic).

Fail-safe: everything before M3b is read-only by construction (`reportWriteProtection` true, writes refused); a hang or
panic is a reboot into Ubuntu.

## 9. Fallback

`RamBlock` + Data seed (plan M2 path). If takeover is refused (MCQ, busy doorbells), the driver logs and does not
publish; the RAM root keeps booting.

## 10. Open questions

1. Does piano's UEFI UFSDxe (stock binary in the pinned Mu platform) leave the controller in SDB or MCQ mode, and with
   doorbells idle at ExitBootServices? → M2 probe 1. If MCQ: can the HLOS switch MCQ off and re-enable SDB without a
   host reset (which would need the PHY cold path)?
2. Does Gunyah leave stream 0x60 in bypass for the HLOS, so ShengUfs's SMMU pass-through write works? (sheng: yes;
   SM8750 13T: yes by inference from the host-verified record.) → M2.
3. One partition with windows or three GPT entries? Decide with the image recipes before M3b.
4. The 13T read timeout after ~300 k reads: is it fixed in b11de87 ("8 concurrent commands, Linux-ufshcd-like
   queueing" per the 27.3 note) or SM8750-specific? → read ShengUfs history; soak at M3a.
5. Do we need any write path to the GPT (slot-success attribute for M4, plan §5)? If so it is a separate, explicitly
   gated write path, not the Data window.

## 11. Licence note

`ufs-qcom.c`/`ufshcd.c` (GPL-2.0) are read as the specification only; the UFSHCI register layout is the JEDEC
standard. ShengUfs and the Odin3 fork are reused under their own licences (check `LICENSE` in each repo before
copying). No Linux code is copied.
