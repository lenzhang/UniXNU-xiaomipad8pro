# Wi-Fi: Qualcomm WCN7860 ("peach", PCI 17cb:110e) on PCIe0: PianoWifi

Status: design + levels 1–4 code, 2026-11-11. `package/PianoWifi` carries level 1 (read-only survey), level 2 (power sequencing),
level 3 (PCIe0 root-complex bring-up to a config-space read of `17cb:110e`, BAR0 and the SMMU stream) and level 4 (MHI: the
firmware over BHI/BHIe to mission mode). All four are built, host-tested (`tests/wifi_host.cpp`, `tests/bringup_host.cpp`) and
linked into the native KC (kernel 0060: "12 native kexts linked, none from outside this repository", PianoWifi 471 pointers), inert without
`piano-wifi`; **nothing above level 1 has run on hardware**, and each level must be read from a boot's log before the next one is
tried (§8.2–§8.5). Levels 5 and up (QMI, WMI, scan, join, the macOS side) are design only. Template: `README.md`.

## 1. Purpose and gate

A real network for native macOS: after the first desktop, `PianoNullEthernet`'s `en0` is not one (time, updates, any on-device
work). Gates nothing in M1–M4; part of M5 (plan §4(d).6, §5). Bluetooth shares the chip's power unit (§10) but is a later page.

The work splits in two very different halves: (a) **the chip**: power sequencing, the PCIe link, MHI, firmware load, QMI, WMI, scan,
association (the Linux ath12k stack's job); (b) **the macOS side**: what interface macOS sees (§5). The finding of this page is
that (a) is a well-trodden path with two UniXNU precedents (UniXNU-sheng, SM8550 + WCN7850, hardware-verified; PaganiWifi, SM8750 +
this very chip on the OnePlus 13T, host-tested only), while (b) has no good option for the macOS Wi-Fi UI: the recommendation is an
Ethernet-like interface plus a supplicant, and a USB-C network as the first network (§5.3).

## 2. Hardware facts

Sources: DT = `out/inputs/piano-boot.dtb` (Ubuntu's running DT); GCC/TCSR/PCI/PHY = mainline `gcc-sm8750.c`, `tcsrcc-sm8750.c`,
`pcie-qcom.c`, `phy-qcom-qmp-pcie.c` (facts only); PAGANI = UniXNU-Pagani-Drivers `drivers/PaganiWifi` 3d570b7 (BSD, same SoC and
chip); SUN = Project-SunUEFI `patches/linux/7.3/` (GPL patches, facts only); INV = the local bring-up notes (`ubuntu-hardware-inventory.md`, not published)
§11; FW = `xiaomipad-8pro-mainline/firmware/piano-wlan.json`. All register values are in `package/PianoWifi/WifiHw.h`.

| Item | Value | Source | State |
|---|---|---|---|
| Chip | WCN7860 ("peach" v2), Wi-Fi 7 + Bluetooth; Wi-Fi is PCIe function `wifi@0` below `pcie@1c00000/pcie@0`, `compatible = "pci17cb,110e"`, bus 1 dev 0 | DT, INV | verified (DT) |
| Linux support | mainline ath12k does **not** know 0x110e (it knows 0x1107 WCN7850, 0x1112 QCC2072); Ubuntu's kernel carries peach support (module `ath12k_wifi7`); SUN adds `ATH12K_HW_PEACH_HW20`: aux_ucode and TME-L downloads, board API 1 only, 256 KiB board | mainline `wifi7/pci.c`, SUN 0003 | verified (source) |
| Root complex | `qcom,pcie-sm8750` + `qcom,pcie-sm8550` (Linux: cfg_1_9_0), 2 lanes; PARF `0x1c00000`, DBI `0x40000000`, ELBI `0x40000f20`, iATU `0x40001000` (0x200/region), config `0x40100000` (1 MiB), MHI `0x1c03000`; memory window `0x40300000` + `0x3d00000` (PCI = CPU), 64-bit `0x4_0000_0000` + 12 GiB | DT | verified |
| PHY | `phy@1c06000`, `qcom,sm8750-qmp-gen3x2-pcie-phy`, QMP **v7** layout (pcs `+0x400`, PCS_STATUS1 `+0x414` bit 6 PHYSTATUS); clocks aux, cfg_ahb, TCSR ref (`0xf204008` bit 0), rchng 100 MHz, pipe | DT, PHY, TCSR | verified (source) |
| Power domains | `PCIE_0_GDSC` `0x6b004`, `PCIE_0_PHY_GDSC` `0x6c000` (bit 31 PWR_ON; CFG at +4); collapse vote `0x5214c` bits 0 / 2 | GCC = PAGANI | verified (source); state at hand-over: **needs-boot** (level 1) |
| Clocks | 12 branches voted in `0x52008` bits 0–6 (slv_axi, mstr_axi, cfg_ahb, aux, pipe, slv_q2a, cnoc_sf_axi) and `0x52000` bits 11, 12, 19, 20, 22; 8 of them hardware-gated (may read CLK_OFF while idle); RCGs aux `0x6b084`, rchng `0x6b068`, pipe mux `0x6b080` | GCC = PAGANI | verified (source) |
| Resets | `PCIE_0_BCR` `0x6b000`, `PCIE_0_PHY_BCR` `0x6c01c` (+ link_down, nocsr, phy 0x6f000.. for the record) | GCC | verified (source) |
| Pins (TLMM) | WLAN_EN gpio16 (pull-down, output low in `wlan-en-state`), SW_CTRL gpio18 (input from the chip), PERST# gpio102 (active low, pull-down), CLKREQ# gpio103 (`pcie0_clk_req_n`, pull-up), WAKE# gpio104 (active low, pull-up); BT_EN is PMIC gpio3 of `pmic@5` (pm8550ve "f") | DT | verified (DT); levels: needs-boot |
| PMU supplies | `qcom,wcn7860-pmu`: vddio `vreg_l3f_1p8`, vddio1p2 `vreg_l2f_1p2`, vddaon `vreg_s5f_0p88`, vdddig `vreg_s4d_0p88`, vddrfa1p2 `vreg_s7i_1p31`, vddrfa1p8 `vreg_s3g_1p86` (RPMh: ldof3, ldof2, smpf5, smpd4, smpi7, smpg3); no other consumer of the six in piano's DT. The PMU's 10 LDO outputs feed the chip's vddaon/wlcx/wlmx/rfa/pcie inputs | DT | verified |
| Voltages the chip needs | piano's own DT ranges of the four SMPSs: S5F 876–1000 mV, S4D 876–1036 mV, S7I 1312–1340 mV, S3G 1864–2000 mV (the two LDOs are fixed at 1800 and 1200); SUN's WCN7861 votes agree but for S3G (1860). Level 2 asks for the range's minimum, which is what the chip needs, and never for more than its maximum | DT, SUN 0001 | verified (DT ranges); the chip's own need: inferred |
| PHY supplies | vdda-phy `vreg_l1f_0p88` (always-on), vdda-pll `vreg_l3g_1p2` (also the UFS PHY's PLL supply: on while NativeUfs runs) | DT | verified |
| Reference / sleep clock | piano's DT names no clock for the PMU; mainline's MTP DT names `RPMH_RF_CLK1`; PAGANI and sheng drive PMK8550 gpio3 (32 kHz "alt_sleep_clk") | DT, mainline, PAGANI | **needs-boot** (open question 3) |
| SMMU | `iommu-map`: requester 0 → SID `0x1400`, requester `0x100` (the chip) → `0x1401`; apps SMMU `0x15000000` (83 banks, 128 pages); the PARF's BDF-to-SID table (`0x2000`, CRC-8 hash, bypass bit `0x2c00`[0]) must route RID 0x100 | DT, PCI, SUN docs | verified (DT); firmware's entries: needs-boot (level 1) |
| Interrupts | MSI via the GIC ITS in Linux (`msi-map`, which XNU does not drive); the controller's own MSI lines SPI 141–148, global SPI 140; sheng (SM8550) takes its MSIs from the DesignWare MSI receiver's SPI line instead of the ITS | DT, sheng `network.md` | verified (DT); polling first |
| Link | sheng: Gen2 x1; PAGANI: Gen3 needs cx/mx at NOM, so cap at Gen2 first | sheng, PAGANI | inferred |
| BAR0 | 2 MiB assumed (MHI registers at the start, chip registers through the window register at BAR+`0x310c` (WCN7850) or `0x3278` (QCC2072-like, PAGANI's choice for 0x110e)) | sheng, PAGANI, mainline | **needs-boot** (level 4) |
| Firmware | stock `modem_a` `image/peach/`: amss20.bin (6930496 B, = ath12k `amss.bin`), phy_ucode20.elf (`m3.bin`), aux_ucode20.elf, regdb_xiaomi.bin (`regdb.bin`), bd_p81.elf (`board.bin`, China-region board, API 1; no `board-2.bin`), qdss_trace_config_v2.cfg; also tmel_peach_20.elf (TME-L, PAGANI) | FW, PAGANI | verified (names, sizes) |
| MAC | the firmware reports all zeros; the unit's address is `wlan0=<12 hex>` in `persist:wlan/wlan_mac.bin` (private: read at run time, never logged or committed) | FW docs | verified (location) |
| UEFI | the piano FD has no PCIe host DXE (only `XhciPciEmulation`): PCIe0 is most likely off at hand-over | `logs/fd/fv-modules.txt` | inferred; level 1 measures it |
| Gunyah | PCIe0 MMIO and the BAR window are reachable from EL1: Ubuntu's ath12k runs from the same EL1 under the same hypervisor | — | inferred (no measurement natively) |

## 3. Ubuntu as the specification

Kernel `yzddmr6/linux-sm8750-piano` `piano/v1.1.0` `d2710cd1` (not checked out on this Mac; the mainline files above serve for
SoC facts): `drivers/pci/controller/dwc/pcie-qcom.c` (`qcom_pcie_init_2_7_0`, `qcom_pcie_post_init_2_7_0`,
`qcom_pcie_config_sid_1_9_0`, `qcom_ep_reset_deassert`: PERST# low ≥ 100 ms, then LTSSM enable), `phy-qcom-qmp-pcie.c`
(`qmp_pcie_power_on`: power-down control, the v7 tables for both lanes, pipe clock, out of reset, start, PHYSTATUS),
`drivers/power/sequencing/pwrseq-qcom-wcn.c` (wcn7850 entry: vregs in DT order, 50 ms power-up delay, WLAN_EN), `drivers/bus/mhi/host`
(BHI/BHIe, EE, channel/event rings), `net/qrtr/mhi.c` (IPCR channels 20/21), ath12k `qmi.c` (WLFW: host cap, indication register,
memory request/response, BDF/regdb/M3/aux download, cal, wlan_cfg, wlan_mode), `htc.c`/`wmi.c`/`dp*.c`, and the peach deltas of the
Ubuntu tree (SUN's 0003 has the same shape). Behaviour only; GPL-2.0, nothing copied (§11). Ubuntu's status: Wi-Fi works, stable
2.4 GHz and 5 GHz 80 MHz links (`xiaomipad-8pro-mainline/README.en.md`).

## 4. UniXNU templates, and what can be borrowed

| Piece | Where | Licence | Use |
|---|---|---|---|
| Stage ladder `wifi=1..6`: survey, rails + WLAN_EN + domains + clocks + PHY + PERST# + LTSSM, endpoint/BAR/MHI, firmware over MHI, the stack | UniXNU-sheng `package/WifiProbe.cpp` 00ba909 (SM8550; on iPadOS 27 hardware-verified through scan, WPA2 association, DHCP and traffic) | BSD-2-Clause-Patent | the shape of the ladder and the read gates (level 1 re-implements them); its SM8550 addresses are not piano's |
| The same ladder for **SM8750 + 0x110e**: GCC/PARF/PHY offsets, the 128-write v7 PHY sequence of the 13T's vendor, BDF-to-SID, DMA window, BAR placement, own MHI boot, RDDM | PaganiWifi 3d570b7 (`PaganiWifiTables.h`, `WifiBringup.cpp`, `MhiBoot.c`, `WifiMhiLoad.c`, `WifiDma.c`) | BSD-2-Clause-Patent | **the main donor for levels 2–4** (with UPSTREAM; what was taken and what was re-expressed: `package/PianoWifi/UPSTREAM`); every value re-checked against piano's DT (its rails/PPIDs/pins are the 13T's) |
| MHI BHI/BHIe boot state machine | sheng `MhiBoot.c/h`, PaganiWifi `MhiBoot.c` | BSD | level 4 (`WifiMhi.h`, re-expressed in this namespace) |
| QRTR v1 + QMI header/TLV codec, name service | `package/PianoPower/QcomQrtr.c/h` (from sheng, hardware-verified on piano's ADSP path) | BSD | level 5 (copied into PianoWifi with UPSTREAM; PianoPower is not modified); the WLFW messages are written fresh |
| cmd-db lookup, RSC layout, RPMh active request | PianoKeyboard `CmdDb.h`, PianoPower `QcomRpmh.c`, PianoA830 `Sm8750Rpmh.h` | BSD / MIT (Adreno830) | level 2 rail votes, **inside the DRV-2 lock `sm8750_rpmh_drv2_owner`** (f53192b) |
| GCC vote lock for `0x52000`/`0x52008` | PianoA830 `Sm8750GpuPower.c` `sm8750_gcc_vote_owner` (weak word, the SoC kit's protocol) | MIT (Adreno830) | level 2: every read-modify-write of the vote words under it (A830 votes GPLL0 in `0x52000`) |
| SMMU bypass bank for a stream | NativeUfs (`SmmuTakeover.h`), PianoDwc3 `SmmuStream.h` | BSD | level 3's tail: stream 0x1401 (`SmmuStream.h`, PianoDwc3's copied with its namespace changed) |
| Firmware read in place from `modem_a` (FAT, read-only through NativeUfs) | PianoPower `PianoAdspFirmware.c` + `FatVolume.c` | BSD | level 4: `image/peach/amss20.bin` at run time (`WifiFirmware.c` + a copy of `FatVolume.c`); nothing staged in any repository |
| Linux ath12k + mac80211 + cfg80211 on XNU, wpa_supplicant 2.12 control path, board callbacks (`nw_abi.h`: platform clock/sleep/log/alloc/DMA/firmware, board BAR/config/MSI) | `unixnu-research/Ath12kWifi` 48e23bf | **GPL-2.0 sources fetched at build time; the repository has no LICENSE** | levels 6–8 only by owner decision (§5.2, §11); WCN7850-only today (no peach) |
| macOS 27 presentation: `ShengEthernet` (an IOEthernetController en0 carrying the Wi-Fi module, vtable-checked) | sheng `package/ShengEthernet.cpp`, `board.toml` macOS 27.3 profile | BSD | level 9, the same model as `PianoNullEthernet` (already vtable-safe in our KC) |

## 5. IOKit family / ABI: the macOS side

Level 1 is a plain `IOService` on `IOPlatformExpertDevice` (`com.apple.kpi.*` only). The real question is what macOS sees later.

### 5.1 What the macOS 27 KC offers

The vma2 KC (`kc27.bin`) carries `IO80211Family` in its Skywalk form (`IO80211Controller`, `IO80211SkywalkInterface`,
`IO80211InfraInterface`, `IO80211InfraProtocol`, `IOSkywalkFamily`) and Broadcom's `AppleBCMWLANCore`/`…BusInterfacePCIe`. No headers
exist for any of it (not in the SDK or KDK); `airportd`, CoreWLAN and the Wi-Fi menu talk to it through private apple80211 ioctls and
Skywalk channels whose layouts change per release.

### 5.2 Options

| Option | macOS Wi-Fi UI | Effort | Verdict |
|---|---|---|---|
| A. A real IO80211 driver (subclass the Skywalk controller/interface, answer airportd's apple80211 requests) | full: Wi-Fi menu, Settings › Wi-Fi, CoreWLAN, Location | months of reverse engineering a private, per-release ABI; sheng's narrower binding (its own `IO80211Controller`/`IO80211Interface`, `SIOC[SG]A80211`, structs tied to iPadOS 24A446) shows the cost even for wifid; third-party Intel drivers that took this road (AirportItlwm) depend on reverse-engineered headers redone at each major release | not now |
| B. **Ethernet-like**: an `IOEthernetController` (`en1`, built in) whose frames are the station's 802.3 view; scan/join through a control endpoint (kern_control) driven by a supplicant and a small CLI (`pianowifi scan / join SSID`), profile under `private/var/...` (mode 600, never in a repository) | none: macOS shows an Ethernet service; DHCP, DNS, routing, Safari, updates all work; no Wi-Fi menu, no AirDrop/Handoff/Location-by-Wi-Fi, no system Wi-Fi password prompt | small on the macOS side (PianoNullEthernet + a control socket; the model of sheng's macOS profile and of itlwm + HeliPort) | **recommended** |
| C. USB-C network first: CDC-NCM/ECM function on PianoDwc3's gadget + an IOEthernetController, the Mac's Internet Sharing upstream | none (wired) | about a week, no firmware, no PCIe (`usb-device-debug.md` §4.2 option 4) | **do first**, as the network while the Wi-Fi ladder is walked |

Under B, the 802.11 station logic lives where the chip stack lives: in the kernel stack (Linux's mac80211 via Ath12kWifi, or our own)
with WPA2/3 key handshakes in a user-space wpa_supplicant (BSD-licensed; Ath12kWifi's `driver_sheng.c` shows the control path).

### 5.3 en0, TimeSync and PianoNullEthernet

TimeSync derives its clock ID from a **built-in** interface's MAC and looks once, at its start (`docs/bringup-debugging.md` §5,
the local notes `loginwindow-launch.md`). The Wi-Fi interface appears late (firmware load takes seconds), so: keep `PianoNullEthernet` as `en0`,
publish Wi-Fi as `en1` with `IOBuiltin` and without `IOPrimaryInterface`, and let the service order prefer the interface with a
link. Later, optionally, publish the Wi-Fi controller early (link down, the unit's MAC read from persist at run time) as `en0` and drop
PianoNullEthernet, after checking that the TimeSync clock ID stays stable. The MAC never appears in a log line or a commit.

## 6. Boot args

| Boot arg | Default | Effect |
|---|---|---|
| `piano-wifi=N` | 0 (probe() declines, nothing mapped) | 1: the read-only survey (loads only); 2: + the power sequence; 3: + the link, BAR0 and the SMMU stream; 4: + the firmware over MHI; above 4: runs level 4 |
| `piano-wifi-gen=N` | 2 | the root port's target link speed (1–3). Gen3 needs cx/mx at NOM, which nothing here votes |
| `piano-wifi-physeq=0` | 1 | apply only the PHY sequence's frame writes (power-down, reset release, start), not the 128-write table: tells "the table is wrong for piano" from "the PHY does not come up at all" |
| `piano-wifi-rcg=0` | 1 | leave the aux / rchng RCG sources as the boot stages left them |
| `piano-wifi-blind=1` | 0 | vote a rail whose PMIC state could not be read (by default level 2 refuses: a voltage sent blind could be below what another consumer holds) |
| `piano-wifi-release=1` | 0 | on a failure, withdraw the enable of the rails this driver enabled itself (and nothing else); by default the votes stay, since a disable could cut a rail Bluetooth needs |
| `piano-wifi-stop=N` | 0 | return after step N (201–205 level 2, 301–311 level 3), so a boot can stop just before the step being studied |
| `piano-wifi-dma=M` | 24 | the level 4 DMA window in MiB (12–64) |
| `piano-wifi-fwtimeout=MS` | 20000 | the deadline of each MHI phase |
| `piano-wifi-smmu=N` | 1 | 0: the SMMU is left alone (level 4 then needs stream 0x1401 to bypass already); 1: the endpoint's stream 0x1401 through a bypass context bank; 2: also the root port's 0x1400 |
| `piano-wifi-spmi=1` | 0 | add the eight supplies' PMIC regulators and the BT_EN / sleep clock pins over the SPMI observer (the only stores: the observer's read commands) |
| `piano-wifi-hwcg=1` | 0 | count a hardware-gated branch that reads CLK_OFF while voted and under HW_CTL as running for the read gates |
| `piano-wifi-smmu=0`, `piano-wifi-cmddb=0` | 1 | skip the SMMU stream scan / the cmd-db lookup |
| `piano-wifi-delay=S` | 0 | wait S seconds before the survey |
| `piano-wifi-repeat=S`, `piano-wifi-samples=N` | 0, 3 | resample the MMIO part every S seconds, N times (one summary line each) |

## 7. Build, kernel collection

`tooling/k27/build-native-piano-wifi.sh` (host tests `package/PianoWifi/tests/wifi_host.cpp` and `tests/bringup_host.cpp`, the
`FatVolume.c` drift check, the objects in `out/native-kexts/PianoWifi`, the import check), descriptor
`tooling/k27/kext/PianoWifi.json` (`com.unixnu.driver.PianoWifi`), listed in `configs/k27-native-kexts.txt`, so
`tooling/k27/build-native-image.sh` links it into every native KC (inert). The descriptor names two libraries, which is why
PianoWifi comes after them in the list: `PianoA830` for the two lock words (`sm8750_rpmh_drv2_owner`, `sm8750_gcc_vote_owner`) and
`NativeUfs` for `native_ufs_part_find` / `native_ufs_part_read` (level 4's firmware read). The four unresolved symbols the import
check reports are exactly those; the KC link resolves them (kernel 0060: PianoWifi 471 pointers).

| File | What |
|---|---|
| `WifiHw.h` | the register facts of all four levels, each with its source |
| `WifiSurvey.h` | level 1: what is read and the gates that keep it off unpowered blocks (no store in the file) |
| `WifiPhySeq.h` | the 128-write QMP v7 PHY sequence (PaganiWifi's, from the 13T's vendor device tree) with its provenance note |
| `WifiBringup.h` | levels 2 and 3 as one portable sequence over callbacks: the writes, their order, read-backs, waits and timeouts |
| `WifiMhi.h` | level 4: the firmware file's layout, the DMA arena, the BHIe vector table, the BHI/BHIe boot state machine, the MHI contexts |
| `WifiFirmware.c/.h` | the firmware read in place from `modem_a` through NativeUfs (ranges only; no file is ever held whole) |
| `FatVolume.c/.h`, `SmmuStream.h`, `SpmiObserver.h` | copies of PianoPower's and PianoDwc3's (`UPSTREAM`; those packages are not modified) |
| `PianoWifi.cpp` | the IOService: the windows, the logged writes, the RPMh transactions, the SPMI reads, the DMA window, BAR0, the levels' reports |

## 8. Bring-up ladder, test plan and effort

RAM boots only, cable to the Mac, owner present, `piano-reset-after` as the backstop. Each level adds writes only after the previous
level's log was read. Effort = agent work + owner boots, assuming the PaganiWifi values hold for piano.

| Level | What | Pass | Effort |
|---|---|---|---|
| 1 (built) | survey: GCC/TCSR/TLMM, PARF/DBI/iATU and PHY only behind their gates, SMMU 0x1400/0x1401, cmd-db; with `-spmi=1` the eight regulators, BT_EN, sleep clock | the log below; nothing written | done; 1 boot |
| 2 (built) power | the six PMU rails by RPMh (mode NPM, the voltage piano's DT asks for, enable; DRV-2 lock), PERST# low, CLKREQ# to its function, WLAN_EN low 150 ms then high + 50 ms, SW_CTRL read. No sleep clock pin is driven (§10.3) | the rails read back enabled at the voted voltages, the UFS PHY's supplies unchanged | done; 1–2 boots |
| 3 (built) link | GDSC release (GCC vote lock), RCGs, branch votes, TCSR clkref, BCR pulses, PARF set-up, the v7 PHY sequence, PHYSTATUS, BDF-to-SID, root port + memory window, PERST# high ≥ 100 ms later, LTSSM, L0 (Gen2 cap), iATU 0 and 1, config read, BAR0, SMMU stream 0x1401 | `17cb:110e` from the endpoint, BAR0 placed | done; 3–5 boots |
| 4 (built) MHI firmware | the DMA window (24 MiB, 36-bit, write-combined), BAR0's MHI block, `image/peach/amss20.bin` read at run time from `modem_a`, BHI (the boot loader), the MHI contexts, BHIe (the second ELF), mission mode; polled, no MSI | EE mission mode (AMSS) | done; 2–4 boots |
| 5 QMI | MHI channels/events, IPCR 20/21, QRTR, WLFW: host cap, memory regions (MiBs, in the arena), board.bin / regdb / M3 / aux_ucode / TME-L, cal, wlan_cfg, mission mode; RDDM dump on a crash | firmware "ready" indication | 2–4 weeks; 4–8 boots |
| 6 WMI | copy engines, HTC, WMI init/ready, HTT, data-path rings | WMI ready event, MAC/caps | fresh: 4–8 weeks; via Ath12kWifi: 1–2 weeks of board glue |
| 7 scan | WMI scan, management RX | the AP list | fresh 1–2 weeks |
| 8 join | open + WPA2-PSK (supplicant), peer/vdev, keys, TX/RX | DHCP lease, ping | fresh 4–8 weeks; Ath12kWifi + peach deltas 2–4 weeks |
| 9 macOS | §5.2 B: IOEthernetController en1 + control socket + CLI | Safari loads a page | 1–2 weeks |

Totals: about 2–3 months to a working station with Ath12kWifi for levels 6–8 (20–30 owner boots); 6–9 months fully licence-clean.
The numbering of the levels above 4 shifted by one when levels 4 (BAR/MHI) and 5 (firmware) of the first plan became one level:
the firmware load needs the BAR and the contexts in the same run, and splitting them meant two boots that each proved nothing.

### 8.1 Level 1 test image

```sh
tooling/k27/build-native-image.sh            # the native KC (PianoWifi linked, inert) and the regular images
PIANO_EXTRA_ARGS="piano-wifi=1 piano-wifi-spmi=1" PIANO_RESET_AFTER=300 python3 tooling/bootimg/build-ram-images.py \
  --fd out/fd/piano-uefi-A-bundled.fd --fd-name A-bundled --kc out/k27/kc-k27-native.bin \
  --trust out/inputs/kernel.macho.trustcache --seed out/inputs/scratch-data.img --adt out/adt/piano-k27.adt --utc "$(date +%s)" \
  --out out/ram-wifi-l1
python3 tooling/bootimg/verify-ram-images.py out/ram-wifi-l1/build-A-bundled.json   # PASS; the image: out/ram-wifi-l1/boot-piano-macos-ram-A-bundled.img
```

Add `piano-usb=2` to read the lines live (`tooling/native/usb-console.py`); otherwise `grep PianoWifi` in the run's
`xnu-console.txt`. Drop `piano-wifi-spmi=1` for a survey with no store at all. Expected (the likely hand-over state: PCIe0 off):

```
PianoWifi: start: piano-wifi=1 spmi=1 smmu=1 cmddb=1 hwcg=0 delay=0 repeat=0 samples=3 (WCN7860 17cb:110e on PCIe0 0x1c00000)
PianoWifi: level 1: loads only, nothing written; SPMI observer read commands for the PMIC part
PianoWifi: stage 1: GCC, TCSR and TLMM (read only)
PianoWifi: GCC pcie0 gdsc ........ cfg ........ (off) phy gdsc ........ cfg ........ (off) collapse vote ........ (pcie0 collapsed, phy collapsed)
PianoWifi: GCC resets: pcie0 ........, pcie0_phy ........, link_down ..., ...
PianoWifi: GCC votes 0x52008 ........ 0x52000 ........
PianoWifi: GCC branches: slv_axi 8....... off, mstr_axi 8....... off, ... phy_rchng 8....... off
PianoWifi: GCC aux rcg ..../.... rchng rcg ..../.... pipe mux ........ (...); TCSR pcie0 clkref ........ (disabled)
PianoWifi: pin WLAN_EN gpio16: cfg ........ io ........: function 0 pull down 2 mA output ..., out 0 in 0   (8 pin lines)
PianoWifi: stage 2: PCIe0 controller: PARF, DBI, iATU (read only, only when powered and clocked)
PianoWifi: controller not read (PCIe0 power domain off): PARF, DBI and iATU not touched
PianoWifi: stage 3: PCIe0 PHY: PCS status (read only, only when powered and clocked)
PianoWifi: PHY not read (PCIe0 PHY power domain off)
PianoWifi: stage 4: SMMU streams 0x1400/0x1401 (read only)
PianoWifi: SMMU idr0 ........ idr1 60000053: N stream-match groups, 83 banks, 128 pages
PianoWifi: SMMU stream 0x1401: no stream-match entry (the firmware left none)      (or: group G smr .. s2cr .. (bypass|translate|fault ..))
PianoWifi: stage 5: AOP command database: the supplies' RPMh resources (read only)
PianoWifi: cmd-db: ldof3 0x....., ldof2 0x....., smpf5 0x....., smpd4 0x....., smpi7 0x....., smpg3 0x....., ldof1 ..., ldog3 ..., rfclka1 ..., SN4 ...
PianoWifi: stage 6: the supplies' PMIC regulators and the BT_EN / sleep clock pins over SPMI (observer reads)
PianoWifi: rail pmu vddio vreg_l3f_1p8 (ldof3): ppid 5c3 apid .. owner . type 04/.. status .. (...) NNNN mV mode .. en .. (...), DT 1800 mV   (8 rail lines)
PianoWifi: pmic pin BT_EN (pm8550ve-f gpio3): ppid 58a type ../.. status .. mode .. ...   (and the sleep clock pin, ppid 0ba)
PianoWifi: SPMI: N observer read commands, 0 refused stores
PianoWifi: summary: pcie0 off phy off branches-off fff wlan_en 0/0 perst 0/0 sw_ctrl 0 link unread ltssm 0xff phystatus unread
PianoWifi: verdict: WLAN_EN low, PCIe0 off, PHY off, link not up: level 2 must do the whole power-up (rails, WLAN_EN, domains, clocks, PHY, PERST#, LTSSM)
PianoWifi: stage 7: done
PianoWifi: done: N MMIO loads, M SPMI observer commands, 0 other stores; P pages mapped, 0 map failures
```

Linux's `wlan-en-state` would read `cfg 00000201` (GPIO, pull-down, 2 mA, output, low); no Linux ran before this boot, so the
pins show what XBL/UEFI or the reset default left. Read from it: whether PCIe0 / its PHY are powered (if yes, the PARF/DBI/PHY lines appear instead), WLAN_EN/PERST#/SW_CTRL levels,
which stream-match entries exist for 0x1400/0x1401, the cmd-db addresses of the six rails (and whether `rfclka1`/`SN4` exist), the
rails' current set points and enables against §2's ranges, the LDO page rule (a rail line ending "(SM8550 page rule)" or "no
regulator found" settles it), and the sleep clock pin's mode. A hang right after "stage 2" or "stage 3" would mean a gate let a read
through to an unclocked block: rerun with `piano-wifi=1` only (no PARF/PHY reads happen when the domain reads off).

Level 1 also feeds the levels above it: `piano-wifi>=2` runs the cmd-db and SPMI stages whatever `piano-wifi-cmddb` /
`piano-wifi-spmi` say, because level 2 needs each rail's RPMh address **and** the PPID its regulator answered on. A rail with
either missing is never voted (`level 2: FAILED at step 201`).

### 8.2 Level 2 (power): one RAM boot

```sh
tooling/k27/build-native-image.sh
PIANO_EXTRA_ARGS="piano-usb=2 piano-wifi=2" PIANO_RESET_AFTER=300 python3 tooling/bootimg/build-ram-images.py \
  --fd out/fd/piano-uefi-A-bundled.fd --fd-name A-bundled --kc out/k27/kc-k27-native.bin \
  --trust out/inputs/kernel.macho.trustcache --seed out/inputs/scratch-data.img --adt out/adt/piano-k27.adt --utc "$(date +%s)" \
  --out out/ram-wifi-l2
python3 tooling/bootimg/verify-ram-images.py out/ram-wifi-l2/build-A-bundled.json
```

What it writes: 18 RPMh commands at most (mode, voltage, enable per rail, one transaction each, under the DRV-2 lock) and six TLMM
words (PERST#, CLKREQ#, WLAN_EN). Nothing else; no clock, no domain, no PCIe register. Expected (the lines that matter):

```
PianoWifi: levels: 1 survey, 2 power, 3 link (gen 2, physeq 1, rcg 1, blind 0, release 0, stop 0, dma 24 MiB, phase timeout 20000 ms)
PianoWifi: rsc: apps DRV-2 id 00030003 (version 3.3: v3 layout), TCS block +0xd00
PianoWifi: step 201: the rails as the PMIC has them (SPMI observer reads)
PianoWifi: step 202: the six PMU rails by RPMh: mode NPM, voltage, enable (DRV-2 lock per transaction)
PianoWifi: rsc: ldof3 (0x.....): 3 commands through TCS 2 completed after 0 ms (status 00010100 00010100 00010100)
   (six such lines, in the order ldof3, ldof2, smpf5, smpd4, smpi7, smpg3)
PianoWifi: step 203: the rails after the votes; the UFS PHY's supplies unchanged
PianoWifi: rail pmu vddaon vreg_s5f_0p88 (smpf5) 0x.....: before read 500 mV mode .. en 00 -> sent mode 7, voltage, enable -> after read 876 mV mode 07 en 80 (rc 0)
PianoWifi: rail phy vdda-pll vreg_l3g_1p2 (ldog3): 1200 mV en 80 -> 1200 mV en 80; never voted (the UFS PHY's and the DSI PHY's PLL: never voted, never lowered)
PianoWifi: step 204: PERST# low, CLKREQ# to its PCIe function
PianoWifi: step 205: WLAN_EN low, then high; the PMU's power-up time; SW_CTRL read
PianoWifi: pins: WLAN_EN cfg 00000201 io 00000002, PERST# cfg 00000201 io 00000000, CLKREQ# cfg 00000007 io ........, SW_CTRL io ........ (in 1)
PianoWifi: level 2: 24 writes (9 kinds, 0 refused); repeated: ...
PianoWifi: level 2: done (rails voted, WLAN_EN high, PERST# low, SW_CTRL in 1)
```

**What the first level 2 boot must show before level 3 is tried**: every rail line reading back `en 80` at a voltage at least the
one voted; the two PHY LDOs unchanged (a `level 2: FAILED at step 203` means one of them, or a PMU rail, came back lower — stop and
read why before any further boot); `WLAN_EN cfg ... io 00000002`; and the system still alive after the run (NativeUfs keeps
writing: the log reaching the end is the proof the UFS path survived the rail votes). `SW_CTRL in 1` is the chip's PMU saying it
came up, which no boot has shown yet on piano: `in 0` is **not** a failure of level 2 (no board documentation says the pin must
rise), but it is the first thing to suspect if level 3's PHY or link step then fails.

Failure meanings: step 201 `a rail's PMIC state could not be read` — the SPMI survey found no regulator at that PPID (the LDO page
rule, §2); `a rail vote was refused` at 201 — no cmd-db resource of that name. Step 202 — the TCS did not complete, or the DRV-2
lock was held for 2 s by PianoA830 / PianoPower (their lines say so); nothing after that rail is voted. Step 203 — a rail dropped.
Step 204/205 — a TLMM word did not read back, which would mean the pin is owned elsewhere (a `qcom,protected-pins` entry).

### 8.3 Level 3 (the link): one RAM boot

`PIANO_EXTRA_ARGS="piano-usb=2 piano-wifi=3"`, everything else as §8.2 (`--out out/ram-wifi-l3`). What it writes, beyond level 2's:
the GCC collapse word (two bits cleared, under the vote lock), the aux and rchng RCG CFG + update bits, the pipe mux (twice: TCXO,
then the PHY), the two branch vote words (bits only set), the TCSR reference gate, two BCR pulses, 12 PARF words, the 128-write PHY
sequence, 256 cleared BDF-to-SID words plus one entry, the root port's bus numbers / memory window / command / link control 2, one
PERST# release, the LTSSM enable, two iATU regions, the endpoint's BAR0 and command, and one SMMU group + bank.

```
PianoWifi: step 301: the PCIe0 and PCIe0 PHY power domains out of collapse
PianoWifi: step 302: the aux/rchng sources, the pipe mux on TCXO, the branch votes, the TCSR reference gate
PianoWifi: step 303: the PCIe0 and PCIe0 PHY block resets, pulsed
PianoWifi: step 304: PARF: root complex, PHY test power-down off, DBI/iATU bases, slave space, PM bits
PianoWifi: step 305: the QMP v7 PHY sequence, the pipe clock from the PHY, PHYSTATUS
PianoWifi: PHY: 128 writes, PCS status1 ........ (ready) after N polls of 100 us
PianoWifi: step 306: PARF BDF-to-SID: requester 0x100 to stream 0x1401
PianoWifi: step 307: the root port: bus numbers, memory window, target link speed
PianoWifi: step 308: PERST# released (>= 100 ms low), then the LTSSM enabled
PianoWifi: step 309: link training to L0
PianoWifi: link: debug0 ........ (LTSSM 0x11 L0) debug1 ........ (up) status ........ (Gen2 x2) after N ms
PianoWifi: step 310: iATU region 0 (configuration window), then the endpoint's identity
PianoWifi: endpoint: id 110e17cb (17cb:110e, the WCN7860) class 02800002 subsystem ........ pcie cap 0x.. msi cap 0x.. after N polls
PianoWifi: step 311: iATU region 1 (the memory window), BAR0's size and address, the endpoint's command
PianoWifi: BAR0: mask ffe00004 -> 2048 KiB at 0x40400000 (value 40400004), command 00100006; iATU ctrl2 80000000/80000000, memory base/limit 43f04030
PianoWifi: SMMU stream 0x1401: ... -> bypass bank N
PianoWifi: level 3: done (L0 at Gen2 x2, the endpoint answers 17cb:110e, BAR0 2048 KiB at 0x40400000)
```

**What the first level 3 boot must show before level 4 is tried**: `PCS status1 ... (ready)`, `LTSSM 0x11 L0` with `debug1 ... (up)`,
`id 110e17cb`, a BAR0 size that is a power of two inside the 61 MiB window (2 MiB is what sheng's WCN7850 had; another size is fine
as long as it is placed), `command ...6` (memory space and bus master), and the SMMU line ending in `bypass bank N` or `left as it
was`. Anything less and level 4 must not be tried: it is the first level that lets the chip read our memory.

Failure meanings: 301 `a power domain did not come up` — the collapse vote is not the whole story on this SoC (an AOP/CESTA owner);
302 `a branch clock did not start` (a vote did not stick, or the root of the RCG is off) / `an RCG update did not finish` (try
`piano-wifi-rcg=0`) / `the PHY reference clock gate did not set`; 304 `a PARF register did not read back` — the window is not
mapped for EL1 by Gunyah (open question 1), and that is the first boot that can tell; 305 `the PHY did not report ready` — the
donor's table is not piano's (try `piano-wifi-physeq=0`, which should then fail the same way: if it does, the fault is before the
PHY — the reference clock or the rails); 309 `no L0 within the timeout` — PERST#/CLKREQ#, the PMU (SW_CTRL in §8.2), or Gen2 being
too fast for what cx/mx are voted at (`piano-wifi-gen=1`); 310 `the endpoint did not answer 17cb:110e` — all ones means the link is
up but configuration requests do not reach the chip (the iATU or the BDF-to-SID entry), `0001` in the low half means it is still
asking for a retry; 311 `BAR0 could not be sized or placed`. A hang (no line after a step) is a bus stall or an EL2 abort: the
breadcrumb `piano_wifi_stage` holds the step number, and the next boot should use `piano-wifi-stop=N` to stop just before it.

### 8.4 Level 4 (the firmware over MHI): one RAM boot

`PIANO_EXTRA_ARGS="piano-usb=2 piano-wifi=4"` (`--out out/ram-wifi-l4`). This level is the first one where the chip reads host
memory. What it writes: the DMA window's contents (the boot loader, the image's 512 KiB segments, the vector table, the MHI
contexts — all in our own 24 MiB buffer), and in BAR0 the MHI context pointers, the host address range, BHI's image address /
size / doorbell, BHIe's vector address / size / doorbell, MHICTRL (M0), and the event ring doorbells.

```
PianoWifi: SMMU stream 0x1401: bypass set (slot N: smr 80001401 s2cr 000000NN)
PianoWifi: dma: 24 MiB at 0x........ (write-combined mapping 0x........), the endpoint's bus addresses are physical (stream 0x1401 bypasses)
PianoWifi: level 4: BAR0 mapped (0x40400000 + 0x200000); MHI version ........, BHIOFF ........, BHIEOFF ........, MHISTATUS ........
PianoWifi: level 4: BHI at 0x..., BHIe at 0x..., EE PBL, state 0x7; chip TCSR version ........ (major 2)
PianoWifi: firmware partition modem_a (LUN 4, NNNNN blocks of 4 KiB)
PianoWifi: firmware: image/peach/amss20.bin on modem_a, 6930496 bytes: boot loader 524288 bytes, a second ELF image 6406208 bytes at 524288
PianoWifi: firmware: in the window: boot loader at 0x........, 13 segments, vector table at 0x........ (13 entries)
PianoWifi: level 4: contexts at 0x........ (channels 128, event rings 2: control at ..., data at ..., command ring at ...)
PianoWifi: level 4: boot begun in phase boot loader over BHI (EE PBL)
PianoWifi: level 4: waiting for READY after N ms (EE PBL, state 0x7)
PianoWifi: level 4: contexts needed after N ms (EE PBL, state 0x1)
PianoWifi: level 4: waiting for the boot loader to run after N ms (EE PBL, state 0x2)
PianoWifi: level 4: image over BHIe after N ms (EE SBL, state 0x2)
PianoWifi: level 4: mission mode after N ms (EE AMSS (mission mode), state 0x2)
PianoWifi: level 4: mission mode after N ms, M steps, 2 context writes, K event acknowledgements; EE AMSS (mission mode), ...
```

**What the first level 4 boot must show before level 5 is designed against it**: `EE PBL` at the start (the chip is in its primary
boot loader — `EE AMSS` already there would mean something else booted it), the firmware line with the 6930496-byte file split at
512 KiB (that is the peach shape: a boot loader and a second ELF), `boot loader over BHI` → `contexts needed` → `image over BHIe`
→ `mission mode`, and the tablet still alive afterwards.

Failure meanings: `the MHI register layout is not usable` — BAR0 does not carry the MHI block where sheng and PAGANI have it;
`no partition` — NativeUfs had not published `modem_a` yet (level 4 does not wait: rerun, or add `piano-wifi-delay=20`);
`BHI reported an error` with an error code — the boot loader refused the image (its address alignment, or the wrong file);
`BHIe reported an error` — the vector table or a segment; `the device reported a system error` / `went to its crash dump
environment` — the firmware started and died (level 5 adds the RDDM dump that says where); `a phase timed out` — the device stopped
answering, which with `MHISTATUS ffffffff` means its PCIe core restarted under us (PAGANI re-applies BAR0 and the decode bits
there; this driver does not yet).

### 8.5 After any failure

Every level leaves the hardware at WLAN_EN low and PERST# asserted, rails still voted (a disable could cut Bluetooth's supply;
`piano-wifi-release=1` withdraws only the enables this driver added). The GCC votes and the SMMU entry are not withdrawn either:
they cost nothing and the next boot is a fresh SoC. Nothing retries in a loop, so a failed boot ends with the log and an idle
tablet, and `piano-reset-after=300` is the backstop.

## 9. Fallback

`piano-wifi=0` (the default): nothing runs. Network meanwhile: PianoNullEthernet's `en0` (no traffic) and, once built, the USB-C
network (§5.2 C).

## 10. Open questions and risks

1. Gunyah stage 2: are PARF/PHY/DBI and the BAR window mapped for EL1 natively (inferred from Ubuntu)? An unmapped access aborts at
   EL2. Level 1 touches them only when powered, so the first real test is level 3. (needs-boot)
2. SMMU: stream 0x1401 → a bypass bank, and the PARF's BDF-to-SID table must route RID 0x100 there; DMA is non-coherent through the
   bypass bank (the NativeUfs/PianoDwc3 write-combined pattern) although the DT says `dma-coherent`. The firmware's memory requests
   (several MiB) and BHI's power-of-two alignment shape the arena (sheng). (level 1 shows the entries; level 3 sets the stream up and level 4 is the first DMA)
3. Which clock the PMU needs on piano: RF_CLK1 (RPMh), PMK8550 gpio3's 32 kHz, or neither (piano's DT names none). Level 2 as built
   **drives no clock at all**: switching the PMK8550 pin on means a PMIC register write, and PianoWifi's only SPMI access is the
   read-only observer channel (its single kind of store is a read command), so that would mean adding a write path to a PMIC this
   driver otherwise only reads. If level 3's PHY or link step fails and SW_CTRL reads 0, this is the first suspect, and the step to
   add then is an `RPMH_RF_CLK1` vote — a cmd-db resource through the VRM path level 2 already has, cheaper and safer than a PMIC
   GPIO write. (level 1 reads the pin, level 2 reports it, nothing drives it)
4. The v7 PHY sequence (`package/PianoWifi/WifiPhySeq.h`): mainline's table is GPL (not copyable); the 128 writes come from
   PaganiWifi (BSD), which took them from the 13T's **running device tree** (`qcom,phy-sequence`), and they differ from mainline's
   tables in five places (both lanes' RX_MODE_01/10_HIGH4 0x8f against 0x9c, 0x5e4 = 0x0f instead of EQ_CONFIG2 at 0x5e0, and two
   writes mainline does not make). Provenance question, left open on purpose: a vendor device tree's sequence and Qualcomm's own
   driver tables share an ancestor at Qualcomm, so "independent of the GPL tables" cannot be established from here. What can be
   said: it is not a copy of them, it reached this repository through BSD-licensed code, and every entry is a register value, a
   hardware fact. If the owner wants the question closed, the sequence can be read at run time from piano's own stock device tree
   instead, and then no repository carries it. `piano-wifi-physeq=0` applies only the frame writes. Gen3 needs cx/mx at NOM, which
   nothing here votes: the target speed stays Gen2.
5. Peach-specific firmware steps: TME-L (`tmel_peach_20.elf` was refused on the 13T: QMI error 83), aux_ucode, board API 1 with the
   China-region `bd_p81.elf` (other regions use another stock board file); Ubuntu's kernel is the behavioural spec.
6. The UFS path. S7I (`vreg_s7i_1p31`, the chip's vddrfa1p2) is **not** piano's UFS vccq parent, although it is the 13T's: piano's
   `ufs@1d84000` takes `vcc` from `vreg_l17b_2p5` and `vccq` from `vreg_l1d_1p2`, neither of them the Wi-Fi chip's. The rails this
   driver must never move are the two PCIe PHY LDOs: `vreg_l1f_0p88` (always-on; also UFS refgen, USB, CSI) and `vreg_l3g_1p2` (the
   UFS **and** DSI PHY PLL supply). Level 2 sends them no command at all, reads both before and after its votes, and fails the level
   if either moved; `piano-wifi-release=1` never touches them either. (verified from piano's DT)
7. Shared state: the DRV-2 lock for every RPMh vote, the GCC vote lock for `0x52000`/`0x52008` and the collapse word (PianoA830
   holds GPLL0 votes there, PianoTouch the QUP branches), the SPMI observer channel per APID (PianoWifi's APIDs are none of
   PianoInput's/PianoPower's/PianoDwc3's), the PMU with Bluetooth (BT_EN on a PMIC GPIO; a later QcaBluetooth must share the rail
   votes, as sheng's `WcnPower.h` does). Every vote of ours only ever adds.
8. Licensing of Ath12kWifi (GPL code compiled into an XNU kext, no LICENSE in its repository): an owner decision before levels 6–8
   use it; it would stay an external build input, never in this repository (§11).
9. Heat and power: Wi-Fi at full power with no thermal driver; sessions stay attended.
10. Level 4's DMA window is 24 MiB of XNU memory below 2^36, mapped write-combined, and it is **never freed**: once the chip has
   been given those addresses, nothing but a reset guarantees it has stopped writing to them. A failed level 4 therefore ends with
   WLAN_EN low, PERST# asserted and the window still held for the rest of the session.

## 11. Licence note

This repository's packages are BSD-2-Clause-Patent. Linux (`pcie-qcom`, `phy-qcom-qmp-pcie`, `pwrseq-qcom-wcn`, `mhi`, `qrtr`, ath12k,
mac80211) and SUN's kernel patches are GPL-2.0: specification only (behaviour, register offsets and bit positions as facts);
no code, comment or table is copied. The PHY init sequence comes from PaganiWifi (BSD) with attribution, not from mainline, and the
provenance question that leaves open is §10.4 and the header comment of `package/PianoWifi/WifiPhySeq.h`.
UniXNU-sheng and PaganiWifi (BSD-2-Clause-Patent) and this repository's own packages are reused with `UPSTREAM` records. Ath12kWifi
compiles GPL sources at build time; if the owner chooses it, it is consumed as an external, pinned build input and its kext is
not distributed. No Qualcomm binary is reverse engineered. Firmware (`LicenseRef-Proprietary-Firmware`, no redistribution) is read
at run time from the tablet's own `modem_a` (or a staged copy under the ignored `out/`), never committed; the MAC address is read
at run time and never logged or committed.
