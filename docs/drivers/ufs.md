# UFS: NativeUfs (ShengUfs lineage) for qcom,sm8750-ufshc

Status: §1-§11 are the design as written on 2026-11-10; since 2026-11-11 NativeUfs is the root disk of native macOS (the
windows on `linux_rootfs`, the Data store's window: [`../data-store.md`](../data-store.md)). §12, the power side (system sleep and
auto-hibernate), 2026-11-11: **built, host-tested, linked into a KC; not run on the tablet**; off unless its boot argument is set.
Template: `README.md`.

## 1. Purpose and gate

Block storage for the macOS root (read-only, verified) and the Data volume (read-write window) on piano's UFS, so
that native sessions stop depending on the RAM seed. Gates **M3** (plan §5). Until M3 the boots use `RamBlock` + the Data
seed, and UFS is probed read-only.

Scope: adopt the link that UEFI left up (no PHY/clock cold init), single-doorbell transfer requests, LUN 0 only for
writes, named windows inside our own partitions. Out of scope: ICE/inline crypto, MCQ, clock gating, writes outside the
Data window. Hibern8 for a system sleep and the controller's auto-hibernate: §12 (opt-in).

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

`piano-disk-identity=0` (NativeUfs and NativeRamBlock, `package/include/PianoDiskIdentity.h`; default 1 since 2026-11-12): the
old identity. By default each root window and the Data disk publish "Protocol Characteristics" (UFS, Internal) and "Device
Characteristics" (vendor Xiaomi, product "UFS 4.0 OS" / "UFS 4.0 SystemOS" / "UFS 4.0 Data" (RAM Data without a store),
solid state), so diskutil lists them as "internal, physical" with Protocol UFS and Solid State Yes, and their media are named
"Xiaomi UFS 4.0 OS Media" etc. (before: "external, physical", "Native OS Media", "Native Owned RAM block storage Media"). Names
only: every lookup goes by volume UUID. The "Disk Image Driver for MacOS X" seen in ioreg is Apple's IOHDIXController (the disk
image driver every Mac has), not one of these disks.

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

## 12. Power: system sleep (`native-ufs-suspend=1`) and auto-hibernate (`native-ufs-ahit=MS`)

Status 2026-11-11: **built, host-tested, linked into a KC with the k27 0063 kernel; not run on the tablet.** Both are off by default:
without them NativeUfs is not in the power tree, never holds its queue and AHIT stays 0, as before. The contract this implements:
[`../power.md`](../power.md) §4 and `package/include/PianoPowerContract.h`. Code: `package/NativeUfs/RootPower.h` (the sequence,
shared with the host harness), `NativeUfs.cpp` (the power-tree join, `setPowerState`, the exports), `RootQueue.h`
(`hostLinkCommand` also takes DME_HIBERNATE_ENTER), `RootCommands.h` (the hold in `hostSlotTake`), `package/NativeRamBlock/PowerHold.h`
(the Data disk's part). Facts are marked as in `../power.md` (**verified**; **spec**: UFSHCI as JEDEC JESD223 describes it and Linux
`ufshcd.c` / `ufs-qcom.c` read as the specification only, nothing copied; **inferred**; **needs-boot**).

### 12.1 Facts

| Fact | State | Source |
|---|---|---|
| CAP `d5c7031f`: UFSHCI 4.0 (VER `400`), bit 23 AUTOH8 set (the controller has auto-hibernate), 32 transfer slots, MCQ capable (bit 30), legacy doorbell available (bit 29 clear) | verified | `NativeUfs: HC cap=` on every boot of 2026-11-10/11 |
| UEFI hands over with AHIT 0 and the link in Hibern8 (TX lane 0 FSM 1); RootQueue.h's DME_HIBERNATE_EXIT brings it to 5 on every boot. UEFI's driver entered Hibern8 itself: the enter and the exit both work on this link and device | verified (exit), inferred (enter) | `LINK tx=1->5 ... ahitBefore=0`; [`../bringup-debugging.md`](../bringup-debugging.md) |
| GCC as UEFI leaves it: UFS_PHY GDSC `fa22f800` (on), AXI `08000001`, AHB `88000003` (hardware-gated), aggre AXI `08000001` | verified | `NativeUfs: GCC gdsc=` |
| AHIT (0x18): bits 9:0 the idle timer, 12:10 its scale (0 1 µs, 1 10 µs, 2 100 µs, 3 1 ms, 4 10 ms, 5 100 ms); 0 turns auto-hibernate off | spec | UFSHCI; `ufshcd.c` (`UFSHCI_AHIBERN8_*`) |
| With AHIT set the controller enters Hibern8 by itself when the timer runs out with no transfer, task or UIC command outstanding, and leaves it for the next doorbell. With AHIT 0 a link in Hibern8 stays there until DME_HIBERNATE_EXIT: a doorbell then never completes | spec | UFSHCI |
| Linux's default with AUTOH8: 150 ms (`ufshcd_init`), so Ubuntu on this tablet most likely runs with auto-hibernate on (with MCQ, and with ufshcd's error handler behind it: host reset and link start-up) | spec; inferred for Ubuntu | `ufshcd.c` |
| DME_HIBERNATE_ENTER (0x17) / EXIT (0x18): UCCS (IS bit 10) with UCMDARG2[7:0] = 0, then UHES (IS bit 6) / UHXS (bit 5), and HCS.UPMCRS (bits 10:8) = 1 PWR_LOCAL; Linux requires exactly PWR_LOCAL of both and waits 500 ms (`ufshcd_uic_pwr_ctrl`, `uic_cmd_timeout`) | spec | `ufshcd.c` |
| Linux enters Hibern8 only with its queue quiesced (system suspend after the SCSI layer's SYNCHRONIZE CACHE; clock gating when idle) | spec | `ufshcd.c` `__ufshcd_wl_suspend`, `ufshcd_link_state_transition` |
| MPHY TX_FSM_STATE (attribute 0x41, TX lane 0) = 1: the lane is in HIBERN8; ufs-qcom checks exactly this of a hibernated link | spec | `ufs-qcom.c` `ufs_qcom_check_hibern8` |
| Before any driver's off, IOPMrootDomain syncs the file systems (`disk_sync_callout` → `sync_internal`) and suspends the user tasks (`tasks_system_suspend`, then `vm_pageout_wait` up to 10 s): little I/O is left to meet the hold | verified | k27 `IOPMrootDomain.cpp` |
| Children in the power tree go to off before their parent and come back after it (the parent's will-change notifies the children and waits for them; the did-change of a power rise notifies them after the parent's `setPowerState`); siblings run in parallel | verified | k27 `IOServicePM.cpp` (`notifyRootDomain`, `notifyChildren`) |
| `setPowerState` returning an error is treated as an acknowledgement (`(result == IOPMAckImplied) \|\| (result < 0)`): a driver cannot cancel a system sleep that way, and k27 0062 has no driver veto (its only abort is a CPU that does not park); k27 0064 adds one, opt-in (`piano-sleep-cancel=1`, `pianopower::cancelSystemSleep` from kOff or PreSleep, `power.md` §3.7); NativeUfs calls it with `native-ufs-suspend-cancel=1` (2026-11-12, §12.3) | verified | k27 `IOServicePM.cpp` `driverSetPowerState`; `patches/k27/0062-*`, `0064-*` |

### 12.2 What NativeUfs holds, and what stays on (the votes are UEFI's)

NativeUfs holds **no** clock, GDSC, regulator, RPMh or interconnect vote: it never writes GCC, TCSR, the QMP PHY or the PMIC
(`GccUfs.h`: read-only probes; it has no RPMh client). Everything below is as UEFI's UFS driver left it and stays so through a
sleep, with or without `native-ufs-suspend`:

| Resource | What | State natively |
|---|---|---|
| GCC branches | `GCC_UFS_PHY_AXI` 0x77018, `_AHB` 0x77028, `_TX_SYMBOL_0` 0x7702c, `_RX_SYMBOL_0` 0x77030, `_UNIPRO_CORE` 0x7706c, `_ICE_CORE` 0x7707c, `_PHY_AUX` 0x770bc, `_RX_SYMBOL_1` 0x770d8, `GCC_AGGRE_UFS_PHY_AXI` 0x770f0 | enabled (the line at the join and at each suspend prints them) |
| GDSCs | `GCC_UFS_PHY_GDSC` 0x77004, `GCC_UFS_MEM_PHY_GDSC` 0x9e000 | on |
| Reference clocks | the device's `ref_clk` (RPMh `LN_BB_CLK3`), the PHY's ref (RPMh CXO), qref (`TCSR_UFS_CLKREF_EN`) | whatever UEFI requested (RPMh votes of its own session, or none) |
| Supplies | vcc `vreg_l17b_2p5`, vccq `vreg_l1d_1p2`, PHY `vreg_l1j_0p91` / `vreg_l3g_1p2` | as UEFI set them |
| Interconnect | `ufs-ddr`, `cpu-ufs` BCM votes | none from NativeUfs; UEFI's, if it made any |

Consequence (**inferred**): Ubuntu found UFS clocks that never reached a zero reference "ultimately blocking CXO shutdown"
(`../ubuntu-history/kernel.md` §2.1); natively every one of them stays enabled, so CXO shutdown (aosd), and possibly CX collapse,
stay out of reach whatever NativeUfs does with the link. Hibern8 saves the link's own power (both M-PHY ends idle, the device's
link layer down) and is the precondition of any gating. Not done here, each a later, separately gated step with its facts
qualified first: the GCC branches and the GDSC gated with the link in Hibern8 (Linux: `ufshcd_setup_clocks` off, ufs-qcom's lane
and device ref clock control), the PHY powered down (only with the link off), the device in UFS-Sleep (START STOP UNIT power
condition 2 to the device well-known LU) with vcc off. Those would be the first writes NativeUfs ever makes to GCC: a gated branch
touched again before it is ungated stalls the bus (the DWC3 lesson, `../power.md` §4).

### 12.3 The system sleep

```
IOPMrootDomain: file systems synced, user tasks suspended, kIOMessageSystemWillSleep
NativeRamBlock off (NativeUfs's power child; only with the Data store open read-write)
  under its gate (no request half done): SYNCHRONIZE CACHE through the store (native_ufs_store_sync)
    ok: new requests wait (held)          failed: native_ufs_power_veto, nothing held (the sleep goes on with UFS up)
NativeUfs off (RootPower.h hostSuspend), every step bounded:
  1 preconditions: no veto, the transport up and not quarantined, no panic          else: abort, nothing touched
  2 hold: no command takes a slot from now on (hostSlotTake waits)
  3 drain: every command admitted before has given its slot back (<= 1000 ms); no doorbell (0x58, 0x78); no fatal IS
  4 SYNCHRONIZE CACHE (slot 0) when a window takes writes (the store, the development build's root windows)
  5 auto-hibernate off: AHIT saved, 0 written and read back
  6 DME_GET TX lane 0: 1 already (the timer's Hibern8): done; else DME_HIBERNATE_ENTER, UHES, PWR_LOCAL, DME_GET = 1
  steps 2-5 fail: AHIT back, the queue released, the link untouched
  step 6 fails: the link back first (as a wake does), then AHIT and the queue
... k27 0062 suspend-to-idle ...
NativeUfs on (hostResume): DME_GET; DME_HIBERNATE_EXIT when 1; DME_GET != 1; NOP OUT through slot 0; AHIT as saved; queue released
  a failed attempt: logged with IS/HCS/TX, retried after 10, 50, 200, 1000 ms (5 attempts, <= ~8 s in all); a quarantined
  transport (a command timed out: the controller may still own its buffers) ends the attempts at once; after the last: panic
  (the root disk cannot come back; every write acknowledged before the sleep is on the medium)
NativeRamBlock on: its held requests released
```

- **The abort is NativeUfs's part only, unless asked.** An aborted suspend puts everything back as it was (queue released, AHIT, the
  link active and proven by a NOP OUT) and the system sleeps with UFS up, which is exactly the behaviour before; the line says so.
  **Since 2026-11-12, `native-ufs-suspend-cancel=1`** (default 0; host-tested, not run) turns the aborts a sleep must not happen in
  into a cancel of the whole sleep (k27 0064's `pianopower::cancelSystemSleep` from the kOff, honoured only with
  `piano-sleep-cancel=1`, `../power.md` §3.7): `RootPower.h` `UfsAbort` records why the suspend was aborted and `ufsCancelWanted`
  takes a power child's veto (the Data store did not sync: data not yet safe), I/O in flight (commands past the 1000 ms drain, or a
  doorbell set with the queue drained), the cache not synced, and a link that cannot go into Hibern8 (a fatal status, AHIT stuck, the
  UIC refusing, the enter failing: undone first, so the link is up again); not a precondition (the transport not up, quarantined, a
  panic in progress: the disk is no worse asleep). The cancel comes after the undo, so the queue runs again at once; the kernel then
  wakes every driver as after an aborted park (`piano.cancelled`). Refused (no 0064, `piano-sleep-cancel=0`): the sleep goes on with
  UFS up, as before. Without the argument such an abort logs that it would have asked. `tests/RootPower.cpp` checks the kind of
  every abort and which of them cancel.
- **The Data disk.** Its store's writes are FUA (`PersistOverlay.h`): every acknowledged write is on the medium before it
  completes; the sync is belt and braces. Holding the disk at its own gate (not only in NativeUfs) keeps a multi-block request (the
  pages, then the bitmap block) from straddling the sleep, so the store at the cut-off is exactly the acknowledged one.
- **Requests in the sleep window** (after the holds and before the freeze, after the thaw and before the wake's on) wait; they run
  after the link is back and never reach a hibernated link (host-tested). The panic path does not wait: a panic in that window
  cannot write through a hibernated link (its commands time out), no loss on this board (no panic log on UFS).
- `native_ufs_dme_get` refuses while the queue is held.
- The order needs NativeRamBlock to be NativeUfs's power child (`native_ufs_power_parent`, `addPowerChild`); with Data volatile it
  does not join and only the root windows are synced (the development build writes them in place).
- Worst cases (failure paths only; the normal case takes milliseconds): off <= ~1 s drain + the sync's 10 s timeout + ~1 s of UIC
  waits; on <= ~8 s before the panic. Both synchronous (`kIOPMAckImplied`), which IOKit allows (it only records a slow call);
  IOPMrootDomain's sleep/wake watchdog is far above.

### 12.4 Auto-hibernate at run time: the evaluation

`native-ufs-ahit=MS` (10 .. 100000; AHIT rounded up to the 1, 10 or 100 ms step; refused without AUTOH8, outside the range, or when
the value does not read back) sets AHIT once the bring-up is done (after the first TEST UNIT READY). A suspend sets it to 0 for the
sleep and the wake restores it.

- **What it saves (inferred until measured):** with the screen on the disk idles nearly all the time between bursts; the link in
  Hibern8 instead of HS-G5 x2 idle saves both M-PHY ends' and the device link layer's power, plausibly tens of mW (1-2 % of the
  ~3 W screen-on budget, `../power.md` §1). It is also the precondition of any later clock gating at run time.
- **What it costs:** the first command after an idle period pays the Hibern8 exit (hundreds of µs to a few ms); the commands'
  timeouts (200 ms and more) cover it many times over. A 1 s timer keeps the exits rare under interactive use; 150 ms is Linux's.
- **Risks:** (1) natively there is **no error recovery**: Linux answers an auto-hibernate enter or exit error (UHES/UHXS with a bad
  UPMCRS, a UIC error) with ufshcd's error handler (host reset, link start-up, the device initialised again); NativeUfs has none
  (that cold path is UEFI's), so such an error makes the next command time out and the transport is quarantined: the root disk is
  gone until a reboot. (2) A software UIC command racing the timer: NativeUfs's only run-time UIC user is `native_ufs_dme_get`
  (local attributes, legal in Hibern8); the suspend writes AHIT 0 before its own. (3) Nothing on this board has run it: UEFI hands
  over with AHIT 0, and Ubuntu's (presumed) use is with MCQ and the error handler. In favour: AUTOH8 is in CAP, and a Hibern8
  enter and exit on this link happen on every boot (§12.1).
- **Recommendation:** test `native-ufs-ahit=1000` (image `ufsahit`: idle desktop, A/B against `base`), then 150; keep it out of the
  daily profile until a long soak (hours of mixed use, a build, a large copy) shows no timeout and the gain is measured.

### 12.5 Boot arguments and lines

| Argument | Default | Meaning |
|---|---|---|
| `native-ufs-suspend=1` | 0 | NativeUfs (and the Data disk, with its store open read-write) in the power tree: §12.3. Exactly `1`; any other value is off |
| `native-ufs-ahit=MS` | none (AHIT 0) | the controller's auto-hibernate after MS ms idle: §12.4 |
| `native-ufs-suspend-abort-test=N` | 0 | a test's switch: suspend N is aborted as "I/O in flight" before anything is held or touched (no hold, drain, sync or UIC command), so the cancel path is proven with the link never in Hibern8 (`../power.md` §6 test C2) |
| `native-ufs-suspend-cancel=1` | 0 | (with `native-ufs-suspend=1`) a suspend aborted for a state the system must not sleep in (Data store not synced, I/O in flight, cache not synced, the link not into Hibern8) asks the kernel to cancel the whole sleep (k27 0064, `piano-sleep-cancel=1`): §12.3 |

```
NativeUfs: power: native-ufs-suspend=1: in the power tree; a system sleep holds and drains the queue, syncs the cache, turns auto-hibernate off and puts the link into Hibern8; any failure first undoes it and the sleep goes on with UFS up
   (with native-ufs-suspend-cancel=1: "... and the kernel is asked to cancel the sleep (native-ufs-suspend-cancel=1)")
NativeUfs: power: suspend 2 aborted (I/O in flight (drain: commands still running after 1000 ms)): asked the kernel to cancel this sleep (native-ufs-suspend-cancel=1): cancelled, nothing is suspended (1 cancelled, 0 refused)
NativeUfs: power: suspend 2 aborted (I/O in flight): a sleep that should not happen; native-ufs-suspend-cancel=1 would ask the kernel to cancel it   (without the argument)
NativeUfs: power: UFS clocks and GDSCs as UEFI left them (NativeUfs holds no vote to drop): GDSC fa22f800, PHY GDSC ..., AXI 08000001, AHB 88000003, UniPro ..., ICE ..., TX symbol 0 ..., RX symbol 0/1 .../..., PHY aux ..., aggre AXI 08000001
NativeRamBlock: power: native-ufs-suspend=1: the Data disk is NativeUfs's power child (...)          (the store open read-write only)
NativeUfs: auto-hibernate: on (native-ufs-ahit=1000): AHIT 00000fe8, the controller takes the link into Hibern8 after 1000 ms without a command and back at the next one
-- per sleep --
NativeRamBlock: suspend 1: store synced in N us (state 2, generation G, P pages present); new requests held
NativeUfs: suspend 1: queue held and drained in 0 ms, cache synced, auto-hibernate was off, link into Hibern8 (TX lane 0 5 -> 1) in N us; N us in all
NativeUfs: suspended: UFS clocks and GDSCs as UEFI left them (...)
IOCPUSleepKernel enter (piano: suspend-to-idle) ... IOCPUSleepKernel exit (piano: timer)
NativeUfs: resume 1: link out of Hibern8 (TX lane 0 1 -> 5), NOP OUT answered, auto-hibernate off, queue released (N commands had waited) in N us
NativeRamBlock: resume 1: requests released (N waits across the sleep)
-- failures --
NativeUfs: suspend N ABORTED at <step>: <why> (IS ... HCS ... doorbells .../...): queue released, link untouched; this sleep goes on with UFS up
NativeUfs: suspend N: Hibern8 enter FAILED: ...: bringing the link back     then     ... ABORTED at Hibern8 enter: ... link up again (TX lane 0 1 -> 5, NOP OUT answered) ...
NativeUfs: resume N: attempt K of 5 FAILED at <step>: <why> (IS ... HCS ..., TX lane 0 ... -> ...); retrying
panic: NativeUfs: the UFS link did not come back (resume N): <step>: <why>; IS ... HCS ... doorbell ...; M commands waited for it. Every write acknowledged before the sleep is on the medium.
NativeRamBlock: suspend: the store's cache sync FAILED (N us): the Data disk keeps running and NativeUfs leaves the link up for this sleep
```

### 12.6 Host tests

`package/NativeUfs/tests/run-tests.sh` (plain and ASan/UBSan) builds the production `RootCommands.h`, `RootQueue.h` and
`RootPower.h` against the register model `tests/UfsModel.h` (ShengUfs's and UniXNU-Odin3's fault fixture, extended with Hibern8
enter, auto-hibernate, the doorbell rule in Hibern8 and the order of events). `RootCommands.cpp`: their command, slot, quarantine,
MCQ-guard and link tests, unchanged in what they assert, with the new code compiled in (no Hibern8 enter, no hold, no AHIT on
today's path). `RootPower.cpp`: the AHIT encoding and refusals; suspend and wake with and without a writable window, with AHIT on
and with the timer's Hibern8; a command issued during the sleep (it waits, runs after the link is back, never into Hibern8); every
abort before the link; every failure at the enter (the link back before the release); an undo or a wake that cannot (five logged
attempts, the backoff, the panic with the queue still held); the panic path. `package/NativeRamBlock/Test/run-tests.sh` `power`:
`PowerHold.h` on a store with a volatile write cache and power cuts.

### 12.7 Build

```sh
package/NativeUfs/tests/run-tests.sh && package/NativeRamBlock/Test/run-tests.sh
K27_KERNEL=$PIANO_K27_TREE/out/k27/kernel-k27-0063-native.macho PIANO_GPU=2 PIANO_VMM_PRESENT=0 tooling/k27/build-native-image.sh
tooling/k27/build-pm-test-images.sh ufssleep0 ufssleep1 ufsahit ufsahitsleep ufsmeasure     # out/ram-pm-<name>, out/ram-pm-dump
```
Built offline 2026-11-11 in a worktree (inputs cloned from the main checkout; the Turnip stage's seed `8f333110`, so Data volatile by
the store guard; main at `9867677`): KC with the 21 native kexts `ec08b6ec615c3a58…` (kernel `ee596f39be1ded3c…`, vtable checks
clean, the native kext rule passed); images `ram-pm-ufssleep0` `c9911d1c…`, `ufssleep1` `21ce6e8b…`, `ufsahit` `fbff2d53…`,
`ufsahitsleep` `0cb35956…`, `ufsmeasure` `66dc3682…`. Rebuild in the main checkout before a run.

### 12.8 The tablet tests (batched; in the order of risk, after `../power.md` §6 S1 and S2 passed)

Every image is a RAM boot with `piano-reset-after` and every reset into ABL; the store follows `build-ram-images.py`'s guard
(another seed: Data volatile, only the root windows take part). The root windows are the installed system's own `linux_rootfs`
windows, written in place by the development build: a UFS failure here touches the real root disk, which is why the abort undoes
everything and why U1 isolates the UFS part with the CPUs in WFI.

| # | Image | Risk | Pass (lines) | On failure |
|---|---|---|---|---|
| U1 | `ram-pm-ufssleep0` (600 s; S1 + `native-ufs-suspend=1`) | medium: the first Hibern8 enter by our code; a wake that fails panics | at boot the two `NativeUfs: power:` lines; per cycle (3) `NativeUfs: suspend N: … link into Hibern8 (TX lane 0 5 -> 1)`, 0062's `woke after 30.0 s by timer`, `NativeUfs: resume N: link out of Hibern8 (TX lane 0 1 -> 5), NOP OUT answered`, the desktop and the USB console back; no `ABORTED`, no `FAILED` | an `ABORTED at <step>` line: the step and the registers say which fact was wrong (the sleep went on with UFS up: compare with S1); a panic or a stall after the wake: the mailbox's last `resume` line; keep `native-ufs-suspend` off |
| U2 | `ram-pm-ufssleep1` (600 s; S2 + it) | as U1, CPUs in ret | as U1; `machdep.piano_sleep` as in S2 | as U1 |
| U3 | `ram-pm-ufsahit` (900 s; `native-ufs-ahit=1000`, no sleep) | medium: run-time Hibern8 with no error recovery | `NativeUfs: auto-hibernate: on (native-ufs-ahit=1000): AHIT 00000fe8`; desktop as `base`, no `UFS ROOT FAILURE`, no I/O timeout; 10 min idle at the login window, then a burst (open apps, a large copy); PianoPower's lines for the A/B with `base` (minutes 2-9, cable out) | a timeout panic or a hung desktop: auto-hibernate stays off; record the last lines |
| U4 | `ram-pm-ufsahitsleep` (600 s) | U1 + U3 | U1's lines with `auto-hibernate off for the sleep` and `auto-hibernate on again`; possibly `already in Hibern8 (auto-hibernate)` (then no enter; the wake still exits) | as U1 / U3 |
| U5 | the installed seed (`PIANO_INSTALLED_SEED`: `build-ram-images.py` then gives `data-persist=2` by itself) with `ufssleep1`'s arguments | **the owner's Data store**: only after U1-U2 passed | U1's lines plus `NativeRamBlock: power: … power child`, per cycle `NativeRamBlock: suspend N: store synced …; new requests held` before NativeUfs's line and `resume N: requests released` after it; `Data store state 2` at the next boot, the files written before the sleep present | a `store's cache sync FAILED`: the sleep went on with UFS up (the veto); stop, keep `native-ufs-suspend` off |
| U6 | `ram-pm-ufsmeasure` (2400 s; `measure` + it), cable out | low once U2 passed | as `measure` (`../power.md` §6.4): the mean current asleep, A/B with `ram-pm-measure`, and the RPMh line at the wake (cxsd/ddr/aosd unchanged or counting) | — |

### 12.9 Risks and limits

- A wake that cannot bring the link back panics after five attempts (~8 s): the root disk is gone either way, and every write
  acknowledged before the sleep is on the medium (FUA store writes, the sync). A link that comes back but misbehaves later shows as
  the usual command timeout (transport quarantined).
- The undo of a failed Hibern8 enter runs the same exit; if that fails too, the same panic.
- No system-level veto exists (§12.3): an aborted UFS suspend still sleeps, with UFS up.
- The GCC/RPMh/ICC votes stay UEFI's (§12.2): Hibern8 alone will not show in aosd; U6 says what it is worth.
- Auto-hibernate: no error recovery natively (§12.4); opt-in, not daily.
- I/O that arrives in the sleep window waits; a kernel thread the sleep itself depends on that did root-disk I/O in that window
  would deadlock the sleep (none known: user tasks are suspended and the file systems synced before the drivers' off; the panic
  path does not wait).
