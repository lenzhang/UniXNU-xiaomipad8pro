# Wi-Fi: Qualcomm WCN7860 ("peach", PCI 17cb:110e) on PCIe0: PianoWifi

Status: design + levels 1–6 code, 2026-11-11. `package/PianoWifi` carries level 1 (read-only survey), level 2 (power sequencing),
level 3 (PCIe0 root-complex bring-up to a config-space read of `17cb:110e`, BAR0 and the SMMU stream), level 4 (MHI: the firmware
over BHI/BHIe to mission mode), level 5 (the chip's IPC router channels, QRTR, and the WLAN firmware's QMI service to its ready
indication) and level 6 (the copy engine's rings over BAR0, the host transport's handshake, the firmware's message interface with
the host's initialisation, and a **passive scan** whose results become the property `piano-wifi-scan`). The macOS side of level 6
is `package/PianoWifi/PianoWifiEthernet.cpp`: an `IOEthernetController` publishing `en1` **link down** with that list
(`tooling/native/piano-wifi` prints it). All six are built, host-tested (`tests/wifi_host.cpp`, `tests/bringup_host.cpp`,
`tests/qmi_host.cpp`, `tests/wmi_host.cpp`) and linked into the native KC ("23 native kexts linked, none from outside this
repository", PianoWifi 604 pointers, PianoWifiEthernet 442 pointers with its vtable check at 0 problem slots), inert without
`piano-wifi`; each level must be read from a boot's log before the next one is tried (§8.2–§8.6, the batched list in §8.8).

**On the tablet (2026-11-12, unattended RAM boots):** level 2 passed (six PMU rails voted through RPMh and read back, `SW_CTRL in 1`:
the PMU came up, 10 writes, none refused); level 3 passed (PHY ready after 6 polls, LTSSM L0 Gen2 x2 after 13 ms, endpoint
`17cb:110e` class 02800001, BAR0 2 MiB, SMMU stream 0x1401 in bypass; 441 writes, none refused). Level 4 and 5 stopped before
the chip was written: `mhiOpen` took the 2 MiB BAR mapping as the register length and refused anything over 1 MiB (BHIOFF 0x100,
BHIEOFF 0x224 were fine): fixed, the mapping now bounds the space (host test added). With `piano-wifi-iova=1` the translating
context bank did not read back what was written (`context bank readback`, before the stream pointed at it): the failure now logs
every register read back against its written value and turns the bank off again. Level 3 held PERST# low 6-18 s (the GCC votes
waited while PianoA830 came up): harmless for the link.

**Second round (2026-11-12 morning, the fixes above):** without the window (`piano-wifi=4`/`5`) MHI opened and the boot began in
the boot loader over BHI, then timed out after 20 s with BHI status `ffffffff` (reads no longer answered) and error `ef0e0003`:
the contexts and the image were at 0x9aab08000, above 4 GiB, which the chip (32-bit DMA, as Linux's driver configures it) cannot
reach; level 4 needs the window (`piano-wifi-iova=1`). With the window, the bank read back everything as written except SCTLR's
ASIDPNE (wrote 0x1027, read 0x27; FSR 0x400 is only its format field): piano's SMMU keeps that bit at 0. Fixed: ASIDPNE is no
longer required, and the window's ASID is its bank number (TTBR0[55:48]) so the shared ASID namespace cannot mix it with another
bank's entries (host test models piano's SMMU).

**Third round (2026-11-12, wifi-l5ic / wifi-l6c with the window working, then offline):** the window translates (`stream 0x1401
translates through bank 81`, the boot loader at bus `0x20000000`) and level 4 fails exactly as before: `EE PBL, state 0, BHI
status ffffffff, error code ef0e0003 (a phase timed out)` after 20 s, so the address width was not the cause. The register offsets,
the order of BHI's writes, the 512 KiB image, its 512 KiB alignment, the window's address being the one the chip gets and the
cache handling were all checked against the MHI host's behaviour and found right (§10.21). Two differences from Ubuntu's path
were found and fixed. (1) **The PCIe NoC's bus clock manager (`SN4`) is never voted natively**: it is the only manager of the
NoC the chip's DMA crosses, it is not keep-alive, pcie-qcom votes it before the controller's clocks, and sheng (SM8550, hardware
verified) votes its own (SN7) before its chip's first DMA; with it off, a DMA read that never completes leaves the boot
loader waiting and BHI's status untouched, which is what both address modes showed. Level 3 now votes it (`piano-wifi-noc=1`, the
default) and every give-up withdraws it. That is the likely cause, **inferred, not proven**: the next boot decides. (2) BHI's
status was never cleared before the doorbell (Ubuntu's MHI host writes 0 to it first: its power-on value is undefined), so
`ffffffff` / `ef0e0003` were most likely the register's untouched power-on contents, not an answer; the driver now clears it,
counts an error (bits 31:30 = 3, all ones included) only when the clear took or the value changed, and reads ERRDBG1–3. Level 4
also dumps the MHI/BHI/BHIe block before the doorbell and at the end, logs every change of MHISTATUS/EE/BHI status/error code/
BHIe status, and after a failure the PCI status and AER words of both ends and the SMMU's global and bank fault registers
(where the DMA went). `piano-wifi-socreset=1` adds the chip-side reset Ubuntu's ath12k runs before MHI. P17 (the GPU's S1 failing
next to level 2) was the DRV-2 lock: one of our transactions slept and logged while holding it and spanned PianoA830's whole 2 s
wait (§10.22); fixed here and in PianoBluetooth's `BtRpmh.h`. Next: §8.8 boots 3a and 3b.

**Fourth round (2026-11-12, wifi-3a `piano-wifi=4 piano-wifi-iova=1` and wifi-3b `piano-wifi=6 … piano-wifi-socreset=1`, both
with every fix above):** SN4 voted and taken, the window translating, and level 4 stops at stage 401 in both, now with the
cause's shape visible: BHI's status read **0** before this host's clear (so the morning's `ffffffff` was not a power-on value),
then **45 ms after the doorbell the boot loader itself wrote `ffffffff` / `ef0e0003`**, with ERRDBG1–3 `20000000 00000000
00080000` — the image's address (low, high) and size exactly as written. EE stayed PBL, MHI state RESET, no SYSERR; both ends'
PCI status, device status and AER clean, PARF only "link up", the SMMU's global fault and bank 81's FSR empty; 3b's partial chip
reset changed nothing. So the PBL refuses the boot loader, and no PCI-level error came with it: either it never read the image,
or every read it made was answered with data (a refused read leaves UR/CA, a lost one a completion timeout, §10.23). The file
is right by structure (checked offline against Ubuntu's pinned `amss.bin`: a signed 32-bit ARM ELF padded to 512 KiB, then the
Hexagon ELF from 512 KiB to the end, which is what the MHI host splits), but whether the window *holds* it was never measured.
What Ubuntu does before that doorbell and this driver did not (§10.23): ath12k's **whole** power-on reset (the chip's LTSSM word,
its GCC hot-reset bit, the **QFPROM's fuse-blow supply bit cleared**, the BHIe vector words — 3b had only the last four steps),
the MHI host's clear of the BHIe receive block, **MSI enabled**, and the MHI context and host-range registers written only at
READY (this driver wrote them, with a 36-bit range, before the doorbell). All of that is now the default, each with an A/B switch,
and level 4 now measures what decides the question: the **TLP counters of both ends** (how many reads the chip sent, how many
completions with and without data came back), the **image hashed through the window as the chip reads it** against the stock
identity, an **MSI sink** (does a chip write reach the window), and BHI's fuse-derived identity words as counts and a digest. The
cause is **not yet proven**: the two candidates the boot loader most plausibly consults are the fuse-blow supply (it checks the
image's signature against fused key hashes) and the early host-range registers (inference). Next: §8.8 boots 4a and 4b.

**Fifth round (2026-11-12, wifi-4a `piano-wifi=6 piano-wifi-iova=1 piano-wifi-band=0` on §10.23's pre-boot path, and wifi-4b, the
old one):** 4b refused as every boot before it (`ffffffff` / `ef0e0003` at +46 ms), now with the counters to say how: the boot
loader **read** the image (21520 reads from the chip, every one answered with data, no UR, CA or completion timeout, the window
holding the stock bytes before and after) and refused it. **4a took it:** BHI complete at +46 ms, READY at 58, the second ELF over
BHIe complete (`8000ec88`, our session) at 426, **mission mode at 477 ms**, 70389 reads served, the chip's writes and its MSI in the
window. So one of §10.23's pre-boot changes cured P18 (which one is an A/B for later: `ctxearly=1`, `socreset=0`, `msi=0` on 4a's
arguments). Then the firmware died. The line after mission mode came out of the console **2027 ms** after the one before it (the
root volume was mounting and launchd starting), the register read after it found **EE RDDM**, level 5's first walk of the control
event ring found the AMSS event followed by a SYS_ERR state change, and the start of channel 20 was never answered. Nothing read the
crash dump (no vector table had been given), so the firmware's reason is not known. What is known is that this host said nothing to
the firmware for 2 s after mission mode, where Ubuntu's MHI host and ath12k assert the wake doorbell, start channels 20 and 21,
answer the router's hello and send the host capability within milliseconds; §10.24 compares the whole window with them, item by
item. Changed (P19, §10.24): the crash dump's table given to the chip before the boot, and read back and decoded on any later death
(`piano-wifi-rddm`, `package/PianoWifi/WifiRddm.h`); the ladder's lines held in memory from the doorbell to its end, so that the
console cannot stall the host while the firmware waits (`piano-wifi-defer`); the device wake doorbell at mission mode and the MHI
host's answers to M0 and M1 (`piano-wifi-wake`); every MHI ring at a multiple of its own length (the data event ring, which
channels 20 and 21 report on, was not); channels 20 and 21 with their fixed fields before the boot and their rings only at their
start; the chip's health read on every turn of levels 5 and 6. The cause is **not proven**; the leading inference is a bounded
wait of the firmware for its host — the host capability is also where this host tells the firmware to use its internal sleep
clock, and natively the PMK8550 sleep clock pin is an input (no 32 kHz is driven to the chip; piano's DT names neither pin nor
clock, so Ubuntu most likely leaves it so too). Next: §8.8 boots 5a and 5b; their crash lines, if any, name the reason.

**Sixth round (2026-11-12 evening, desk-dev13w/14/15 with `piano-wifi=6 piano-wifi-iova=1` on the desktop image):** level 4 as
in 4a — mission mode at 481 ms (BHI `80000567`, BHIe `80000568`, `cafecace` the boot loader's hand-over word, EE AMSS), 70517
reads served, the MSI in the sink, the wake doorbell asserted — and this time the firmware **lived**: no RDDM, no system error in
the 11 s level 5 then ran (with the console kept off the ladder's thread, P19's 2 s death did not happen; inference made evidence
by three boots). Both channels started (`states 0x2 / 0x2` 4 ms into level 5), but `0 receive buffers posted`, no hello from the
chip, ours unanswered, no service: stage 502. **Cause (P20, §10.25, proven by the code and its host test):** the event walk after
channel 20's start posted the receive buffers into channel 21's ring before 21 was prepared, and 21's preparation then put that
ring's write pointer back at the base with every buffer still marked busy — the chip's router had nowhere to put its hello or its
answer to ours. Behind it a second fault no boot had reached yet: every transfer completion was matched against the buffers'
addresses, but the MHI event names the ring **element** it completes, so not one packet in or out would have been recognised.
Changed: buffers only on a running channel 21, posted right after its start; per-ring element-to-buffer records and a host read
pointer walked to the element the event names (merged completions, overflows, doorbell-mode events handled); the hello's own
completion and both channels' and the data event ring's pointers in the log; the held lines printed by a helper thread at every
phase change and within a second (a hung ladder can no longer stay silent, desk-dev8n) and a hard deadline for each level
(`piano-wifi-deadline`). Next: §8.8 boot 6a.

**Seventh round (2026-11-12 21:18, the RAM image `ram-v32-probe`, and 21:31, the installed ESP v3.2a, both `piano-wifi=6
piano-wifi-iova=1`):** level 4 as before (mission mode at 483 ms), and level 5 now **works up to the downloads**: 63 receive
buffers posted, the chip's hello answered, service 69 instance `0x101` on port 1, the radio's capability (2 radios), the
indication registration, the host capability, the firmware's memory request (3 regions, 11284 KiB: host memory 7232 KiB, the
calibration database 4048 KiB, 4 KiB shared with the audio DSP, all in the window) answered and its "memory ready", the chip's
capability (chip `0x2` family `0x4` SoC `0x40210200`, board `0xff`, firmware `0x100d002f` built 2025-08-26). Then step 509, the
regulatory database (`regdb_xiaomi.bin`, 24278 bytes: four segments, the last 5846 bytes): the first three answered and
accepted, the fourth (the last) never answered; `a step timed out` after 10 s, stage 504. Both boots identical, down to `11
packets out` and `10 data` packets in. **Cause (P21, §10.26; the count is evidence, the mechanism inferred from the router
protocol and reproduced by the host test):** the firmware's router asks for a flow-control answer (RESUME_TX) on its fifth
packet to a port and sends nothing more to it from its tenth until one names that port; the router this driver copied answered
with the *sender's* address, from the control port, which resumes nothing, so the firmware's eleventh packet to us (the answer to
the last segment) stayed in its queue. The `11 packets out` were the hello, nine requests and that one misaddressed answer. Not
the cause: the file (it is the one Ubuntu sends as `regdb.bin`, same size), its message (kind 4, file identifier `0xff`, the
remaining size, the end flag on the last segment: Ubuntu's form), the board identifier (`0xff` is what this unit's OTP gives, and
Ubuntu sends `0xff` too), TME-L (Ubuntu's kernel for this tablet sends none before the regulatory database and stages no such
file). Changed: RESUME_TX goes from our port to the sender's node:port naming our node and port (net/qrtr's form), an answer the
link refuses is sent again; every QMI answer and indication is logged with its result and error codes (and the segment it
answers), the request outstanding at a timeout is named, the flow-control counters have their own line, the board file's choice
for board `0xff` is logged (`bd_p81.elf`, the stock API-1 default board file: Ubuntu's choice with no board-2 container), its kind
is taken from its ELF magic, and the trace configuration can be refused or go unanswered without ending the level. Next: §8.8
boot 7a.

**Eighth round (2026-11-12 23:33, the installed ESP v3.3d, `piano-wifi=6 piano-wifi-iova=1`): level 6 reached.** Level 5
finished (the firmware ready at +675 ms, P21's fix working), the host transport came up (2 credits of 2176 bytes, 5 endpoints;
our endpoint 1 on pipes 3 out / 2 in), the message interface connected, the capability event and two extended ones read, the
initialisation accepted and the firmware's **ready event at +829 ms** (status 0, 16 peers, its own address all zeros and one
extra address announced). Then the virtual device (+829 ms) and the passive scan (+929 ms), and at **+1045 ms the firmware
asserted**: EE RDDM, MHISTATUS SYSERR, the scan never started. P19's crash-dump path worked on its first real use and gave the
firmware's own reason out of `Q6-SFR.bin` — `cmnos_assert_patched.c`, line 488 — with every bus-level register clean. **Cause
(P22, §10.27, inferred from Ubuntu's order):** nothing was said to the radio between the ready event and the device, where
Ubuntu's driver sends the hardware mode, six radio parameters and the channel list of the firmware's own regulatory event
first, and never creates a device with an all-zero address. All four are in behind `piano-wifi-l6pre` (default on), the
regulatory event is read and turned into the channel list, and the log now names the last command before a death and every
unknown event. Next: §8.8 boots 8a and 8b.

**Ninth round (2026-11-13 02:05, the SPMI-lock probe, the same arguments): no assert, the scan complete, no frame.** P22's
set-up all accepted (hardware mode, 6 parameters, country CN with 4 rules into a 26-channel list, the device with piano's own
address), the scan started, visited all 22 channels and completed — and **0 management frames** reached the host. **Cause
(P23, §10.28, inferred from the chip's own host):** this family's receive DMA has no buffers of its own; the host must fill a
refill ring the firmware hands on to the radios. Built: the data interface, its version, the six receive rings with their
buffers, and a second scan after them on the same boot (the first is the control), with the channel visits' counters and the
radio's statistics to tell "no buffers" from "a deaf radio". The scan lines no longer print a network's name. Next: §8.8 boot 9a.

**Level 7, the join, is built (2026-11-13, `piano-wifi=7`, §13.3; not yet run on the tablet).** Level 6 keeps its virtual
device and the transport after the second scan and hands them over; level 7 waits for the join profile (`machdep.piano_wifi_join`),
chooses the profile's network out of the scan list (the strongest access point of the name, a 5 GHz one first), and joins in this
chip's own order: the station's parameters, the peer, its HTT peer map, its default routing and 17 reorder queues, the start on
the network's channel, Open System authentication and the association request through `WMI_MGMT_TX` with their answers as
`WMI_MGMT_RX`, the peer's association (WPA2: the four-way handshake flag), the station up. The log carries steps, status codes,
the AID and lengths, never a name, an address or a key; en1 now carries the virtual device's own address. The keys (the
four-way handshake in user space) and the data path are the next two pieces; until the keys come, a WPA2 access point ends the
association after a few seconds, and level 7 logs how. Host-tested in `tests/sta_host.cpp` against a firmware and access-point
model. Next: §8.8 boot 10a.

Level 7's **messages** are built and host-tested (`package/PianoWifi/WifiAssoc.h`, `tests/assoc_host.cpp`): the regulatory
rules, `WMI_SCAN_CHAN_LIST`, `WMI_VDEV_START`/`UP`/`STOP`/`DOWN`, `WMI_PEER_CREATE`/`ASSOC`/`SET_PARAM`,
`WMI_VDEV_INSTALL_KEY` and `WMI_MGMT_TX`; the step machine that drives them, the four-way handshake and the path the
passphrase takes are §13.2's plan and wait for level 6 to pass on the tablet. The data path is level 8 (§13). Template:
`README.md`.

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
| MHI channels (level 5) | the chip's IPC router channels 20 (to the device) and 21 (back), 64 ring elements each, both on the second event ring (256 elements, vector 2), burst mode disabled, 8 KiB per receive buffer; the channel doorbells at the offset `CHDBOFF` reports, the command ring's at `0x70` | Ubuntu's `wifi7/mhi.c`, the MHI specification's names | verified (source) |
| The firmware's QMI service | service 69, version 1, instance 1 (announcement word `0x101`); the client identifier the host gives is `0x4b4e454c`; its message set and tags: section 12 | Ubuntu's `ath12k/qmi.[ch]` | verified (source) |
| Copy engine, shadow registers | nine pipes and fourteen service-to-pipe entries (section 12), the board file capped at 256 KiB, 40 shadow registers from BAR0 + `0x8fc`, the ring head pointers in the units at `0x00a38000` (reorder), `0x00a44000` (transmit), `0x00a34000` (buffer manager) and `0x01b80000` and up (copy engines) | Ubuntu's `wifi7/ce.c`, `wifi7/hw.c`, `wifi7/hal*.c` | verified (source); what the firmware accepts: needs-boot (level 5) |

## 3. Ubuntu as the specification

Kernel `yzddmr6/linux-sm8750-piano`, branch `piano/dev` `7f8d6bedde479a01f4a05d6d0581b43c7fd20a01` (the branch `piano/v1.1.0`
`d2710cd1` is the released one; the mainline files above serve for SoC facts). Level 5's facts come from this tree, which is why
its commit is pinned here and in `package/PianoWifi/UPSTREAM`: `drivers/pci/controller/dwc/pcie-qcom.c` (`qcom_pcie_init_2_7_0`, `qcom_pcie_post_init_2_7_0`,
`qcom_pcie_config_sid_1_9_0`, `qcom_ep_reset_deassert`: PERST# low ≥ 100 ms, then LTSSM enable), `phy-qcom-qmp-pcie.c`
(`qmp_pcie_power_on`: power-down control, the v7 tables for both lanes, pipe clock, out of reset, start, PHYSTATUS),
`drivers/power/sequencing/pwrseq-qcom-wcn.c` (wcn7850 entry: vregs in DT order, 50 ms power-up delay, WLAN_EN), `drivers/bus/mhi/host` and
`drivers/bus/mhi/common.h` (BHI/BHIe, the execution environments, the channel, event and command rings, the doorbell order),
`net/qrtr` (the version 1 header, the control messages, and that the chip's router channels are 20 and 21), ath12k `qmi.c` and
`qmi.h` (the message set of §12 with every tag, the order of the exchange, the memory types and the "ask again smaller" fallback),
`wifi7/mhi.c` (this chip's channel and event configuration), `wifi7/ce.c`, `wifi7/hw.c`, `wifi7/hal*.c` (the copy engine's pipes,
the services' pipes, the board-file cap, the shadow registers and the ring register map), `drivers/soc/qcom/qmi_encdec.c` (how a
counted array and a string are encoded), `htc.c`/`wmi.c`/`ce.c`/`hal*.c`/`mac.c` (level 6's wire facts, §14) and `dp*.c` (level 8's shape, §13), and the peach deltas of the Ubuntu tree
(SUN's 0003 has the same shape). Behaviour only; GPL-2.0, nothing copied (§11). Ubuntu's status: Wi-Fi works, stable
2.4 GHz and 5 GHz 80 MHz links (`xiaomipad-8pro-mainline/README.en.md`).

## 4. UniXNU templates, and what can be borrowed

| Piece | Where | Licence | Use |
|---|---|---|---|
| Stage ladder `wifi=1..6`: survey, rails + WLAN_EN + domains + clocks + PHY + PERST# + LTSSM, endpoint/BAR/MHI, firmware over MHI, the stack | UniXNU-sheng `package/WifiProbe.cpp` 00ba909 (SM8550; on iPadOS 27 hardware-verified through scan, WPA2 association, DHCP and traffic) | BSD-2-Clause-Patent | the shape of the ladder and the read gates (level 1 re-implements them); its SM8550 addresses are not piano's |
| The same ladder for **SM8750 + 0x110e**: GCC/PARF/PHY offsets, the 128-write v7 PHY sequence of the 13T's vendor, BDF-to-SID, DMA window, BAR placement, own MHI boot, RDDM | PaganiWifi 3d570b7 (`PaganiWifiTables.h`, `WifiBringup.cpp`, `MhiBoot.c`, `WifiMhiLoad.c`, `WifiDma.c`) | BSD-2-Clause-Patent | **the main donor for levels 2–4** (with UPSTREAM; what was taken and what was re-expressed: `package/PianoWifi/UPSTREAM`); every value re-checked against piano's DT (its rails/PPIDs/pins are the 13T's) |
| MHI BHI/BHIe boot state machine | sheng `MhiBoot.c/h`, PaganiWifi `MhiBoot.c` | BSD | level 4 (`WifiMhi.h`, re-expressed in this namespace) |
| QRTR v1 + QMI header/TLV codec, name service | `package/PianoPower/QcomQrtr.c/h` (from sheng, hardware-verified on piano's ADSP path) | BSD | **level 5 (built)**: copied as `WifiQrtr.c/h` with two additions (a packet built in the transport's buffer, a service matched on its instance word; `UPSTREAM`). PianoPower is not modified; the WLFW messages, the channels and the QMI writer are fresh |
| A translating SMMU context bank with a table of ours, and the plan/apply split for a stream with no bank of its own | `package/PianoDpu/DpuSmmu.h`, `package/PianoUsbHost/SmmuIdentity.h` | BSD | **level 5 (built)**: `WifiSmmuWindow.h`, the 32-bit window of §10.11 (`piano-wifi-iova=1`); neither package is modified |
| cmd-db lookup, RSC layout, RPMh active request | PianoKeyboard `CmdDb.h`, PianoPower `QcomRpmh.c`, PianoA830 `Sm8750Rpmh.h` | BSD / MIT (Adreno830) | level 2 rail votes, **inside the DRV-2 lock `sm8750_rpmh_drv2_owner`** (f53192b) |
| GCC vote lock for `0x52000`/`0x52008` | PianoA830 `Sm8750GpuPower.c` `sm8750_gcc_vote_owner` (weak word, the SoC kit's protocol) | MIT (Adreno830) | level 2: every read-modify-write of the vote words under it (A830 votes GPLL0 in `0x52000`) |
| SMMU bypass bank for a stream | NativeUfs (`SmmuTakeover.h`), PianoDwc3 `SmmuStream.h` | BSD | level 3's tail: stream 0x1401 (`SmmuStream.h`, PianoDwc3's copied with its namespace changed) |
| Firmware read in place from `modem_a` (FAT, read-only through NativeUfs) | PianoPower `PianoAdspFirmware.c` + `FatVolume.c` | BSD | level 4: `image/peach/amss20.bin` at run time (`WifiFirmware.c` + a copy of `FatVolume.c`); nothing staged in any repository |
| Linux ath12k + mac80211 + cfg80211 on XNU, wpa_supplicant 2.12 control path, board callbacks (`nw_abi.h`: platform clock/sleep/log/alloc/DMA/firmware, board BAR/config/MSI) | `unixnu-research/Ath12kWifi` 48e23bf | **GPL-2.0 sources fetched at build time; the repository has no LICENSE** | levels 7 and 8 only by owner decision (§5.2, §11); WCN7850-only today (no peach); levels 1-6 need none of it |
| macOS 27 presentation: `ShengEthernet` (an IOEthernetController en0 carrying the Wi-Fi module, vtable-checked) | sheng `package/ShengEthernet.cpp`, `board.toml` macOS 27.3 profile | BSD | the shape of `PianoWifiEthernet` (§13.1), which is the same model as `PianoNullEthernet` (already vtable-safe in our KC) |

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
| B. **Ethernet-like**: an `IOEthernetController` (`en1`, built in) whose frames are the station's 802.3 view; scan/join through a control endpoint (kern_control) driven by a supplicant and a small CLI (`piano-wifi`, `join SSID`), profile under `private/var/...` (mode 600, never in a repository) | none: macOS shows an Ethernet service; DHCP, DNS, routing, Safari, updates all work; no Wi-Fi menu, no AirDrop/Handoff/Location-by-Wi-Fi, no system Wi-Fi password prompt | small on the macOS side (PianoNullEthernet + a control socket; the model of sheng's macOS profile and of itlwm + HeliPort) | **recommended; its skeleton is built** (`PianoWifiEthernet`, §13.1: `en1` link down with level 6's scan list, no frame yet) |
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
| `piano-wifi=N` | 0 (probe() declines, nothing mapped) | 1: the read-only survey (loads only); 2: + the power sequence; 3: + the link, BAR0 and the SMMU stream; 4: + the firmware over MHI; 5: + the IPC router channels, QRTR and the firmware's QMI service; 6: + the copy engine, the host transport, the firmware's message interface and a passive scan (and `piano-wifi-mode` defaults to 1, since level 6 needs the firmware's radio running); 7: + the join on level 6's virtual device (§13.3); above 7: runs level 7 |
| `piano-wifi-gen=N` | 2 | the root port's target link speed (1–3). Gen3 needs cx/mx at NOM, which nothing here votes |
| `piano-wifi-physeq=0` | 1 | apply only the PHY sequence's frame writes (power-down, reset release, start), not the 128-write table: tells "the table is wrong for piano" from "the PHY does not come up at all" |
| `piano-wifi-rcg=0` | 1 | leave the aux / rchng RCG sources as the boot stages left them |
| `piano-wifi-blind=1` | 0 | vote a rail whose PMIC state could not be read (by default level 2 refuses: a voltage sent blind could be below what another consumer holds) |
| `piano-wifi-release=1` | 0 | on a failure, withdraw the enable of the rails this driver enabled itself (and nothing else); by default the votes stay, since a disable could cut a rail Bluetooth needs |
| `piano-wifi-stop=N` | 0 | return after step N (201–205 level 2, 301–311 level 3), so a boot can stop just before the step being studied |
| `piano-wifi-dma=M` | 24 (64 at level 5) | the DMA window in MiB (12–96, rounded down to whole 2 MiB blocks). Level 5 also carries the firmware's regions, the two microcodes and the router's buffers in it |
| `piano-wifi-fwtimeout=MS` | 20000 | the deadline of each MHI phase |
| `piano-wifi-qmitimeout=MS` | 10000 | the deadline of each level 5 request (the service's own request timeout): it starts again at every step and at every answer or indication, so a download of many segments gets it per segment; the level's hard deadline (`piano-wifi-deadline`) still bounds the whole. At the trace configuration (step 513) it gives that step up instead of ending the level |
| `piano-wifi-iova=1` | 0 | give the endpoint's stream 0x1401 a translating context bank whose 32-bit window (`0x20000000` + the window's size, 2 MiB blocks) maps the DMA window, and hand the chip those addresses instead of physical ones. The only way to give this chip a 32-bit address, which its own driver uses for the data path's rings (§10.11); the page table is a buffer of its own, outside the window the chip can reach |
| `piano-wifi-mode=1` | 0 | level 5 also sends the firmware's options (logging), the host's copy-engine configuration and the mode — the three messages that start the radio and open level 6. By default level 5 stops at the ready indication |
| `piano-wifi-shadow=1` | 0 | put the shadow register list in that configuration (40 addresses computed from the chip's register map for the rings level 8's data path will create; level 6 writes the real registers through the window instead, §10.18, and §10.19 is what to try if that turns out to be the wrong path) |
| `piano-wifi-noc=0` | 1 | no vote for the PCIe NoC's bus clock manager `SN4` before level 3's clocks. By default level 3 votes it at the link's peak (Gen × 2 lanes, as pcie-qcom does), and every level that gives the chip up withdraws it (§10.21). `=0` is only the A/B test of that vote |
| `piano-wifi-socreset=0` | 1 (from the fourth round; 0 before) | skip the chip's power-on reset before BHI. By default level 4 runs all of ath12k's power-up recipe: the chip's PARF LTSSM word (`0x1e081b0` := `0x111`), its GCC hot-reset bit (`0x1e65304` bit 4), the PCIe interrupts cleared, the QFPROM power control's VDD4BLOW bit (`0x1f8031c` bit 2) cleared, the BHIe vector doorbell/status words, the Q6 cookie read, the warm-boot entry cleared, the reset cause read, the SoC global reset (BAR0 + `0x3008` bit 0, 10 ms each way), MHICTRL's reset; then the MHI host's clear of the BHIe receive block. Two lines with every value (§10.23) |
| `piano-wifi-msi=0` | 1 | leave the endpoint's MSI off. By default one vector, its address a sink page of the DMA window, its data `0x4d53`, INTx off, as the host boots the chip with MSI on; the sink is reported after the boot (§10.23) |
| `piano-wifi-ctxearly=1` | 0 | also write the MHI context and host-range registers before the doorbell (what every boot before the fourth round did). By default they are written once the device reports READY, as the MHI host does |
| `piano-wifi-ras=0` | 1 | no TLP counters. By default level 4 switches on the DesignWare RAS DES counters of both ends of the link before the doorbell (TLPs by kind, UR, CA, completion timeouts) and logs their deltas (§8.4) |
| `piano-wifi-rddm=0` | 1 (from the fifth round) | no crash dump. By default level 4 gives the chip the crash dump's vector table before the boot loader's doorbell (nine 512 KiB segments of the DMA window, 0x420000 bytes as this chip's host reserves, right after the receive block's clear, as the MHI host does), and when the firmware reports its crash dump environment or a system error at any level the dump is waited for (at most 4 s) and decoded into a few lines: the header's files, the firmware's failure reason, nothing else (`package/PianoWifi/WifiRddm.h`, §10.24) |
| `piano-wifi-defer=0`, `=2` | 1 (from the fifth round) | print every line when it is written. By default the ladder's lines from the boot loader's doorbell on are held in a heap buffer (480 lines, 320 before the seventh round; more are counted, not kept; a crash prints them at once and its own lines directly), each with `[+N ms]` since the doorbell, and printed by a helper thread (sixth round): at every phase change (a stage, a level 4 phase, a level 5 or 6 step) and any line held for a second, so the console stalls the helper and never the ladder (wifi-4a: the console held the ladder's thread for 2 s right after mission mode and the firmware was dead when it came back, §10.24), and a ladder that hangs still has its lines out (desk-dev8n: printed only at the ladder's end, which never came). `=2`: held and printed only when the ladder stops, with no helper and no deadline watch (the fifth round's way: the A/B if the helper's console traffic is ever suspected) |
| `piano-wifi-deadline=MS` | 60000 (from the sixth round) | the hard deadline of each of levels 4, 5 and 6 as a whole (level 6: at least its own overall bound and 5 s), 10000 to 600000. Every wait inside a level has its own shorter bound; this one ends the level whatever it waits for (a `hard deadline … passed` line, then the level's usual failure), and the helper thread prints `watchdog: level N is … ms past its hard deadline` (every 30 s) when the ladder's thread does not come back from it |
| `piano-wifi-wake=0` | 1 (from the fifth round) | no device wake doorbell. By default mission mode asserts it (channel 127's doorbell = 1), as the MHI host's mission-mode transition does, it stays asserted while the levels run and is released when the ladder ends well; an M1 the chip reports anyway is answered with M2 and a wake toggle, an M0 by ringing every event ring and running channel again |
| `piano-wifi-smmu=N` | 1 | 0: the SMMU is left alone (level 4 then needs stream 0x1401 to bypass already); 1: the endpoint's stream 0x1401 through a bypass context bank; 2: also the root port's 0x1400 |
| `piano-wifi-spmi=1` | 0 | add the eight supplies' PMIC regulators and the BT_EN / sleep clock pins over the SPMI observer (the only stores: the observer's read commands) |
| `piano-wifi-hwcg=1` | 0 | count a hardware-gated branch that reads CLK_OFF while voted and under HW_CTL as running for the read gates |
| `piano-wifi-smmu=0`, `piano-wifi-cmddb=0` | 1 | skip the SMMU stream scan / the cmd-db lookup |
| `piano-wifi-delay=S` | 0 | wait S seconds before the survey |
| `piano-wifi-repeat=S`, `piano-wifi-samples=N` | 0, 3 | resample the MMIO part every S seconds, N times (one summary line each) |
| `piano-wifi-wmitimeout=MS` | 10000 | the deadline of each level 6 step (the firmware's own service-ready timeout is five seconds) |
| `piano-wifi-scanwait=MS` | 12000 | how long the scan itself may take. When it runs out and the scan had started, what it found stands and the teardown runs; when it had not started, the level fails |
| `piano-wifi-band=N` | 1 | 1: the scan visits the 2.4 GHz band's 13 channels and the 5 GHz band's 9 non-radar ones; 0: only 2.4 GHz (the shorter boot, and the one that needs no regulatory answer for the upper band) |
| `piano-wifi-bssid=1` | 0 | print a network's whole address in the log. By default only its first three bytes are printed and the rest is `**:**:**`, so that a pasted log cannot carry the owner's environment (§11). The registry property always carries the whole address, which is what a later join needs |
| `piano-wifi-l6pre=N` | `0x1bf` = 447 (P22 §10.27, P23 §10.28; a decimal value is always safe to write) | the radio's set-up between the firmware's ready event and level 6's virtual device, as a bitmask: `0x01` the hardware mode (`WMI_PDEV_SET_HW_MODE`, two bands at once), `0x02` the radio's six parameters, `0x04` the channel list of its regulatory event (`WMI_SCAN_CHAN_LIST`; this driver's own list when no event came), `0x08` a usable device address (never all zeros), `0x10` wait for the regulatory event before the list, `0x20` pace the commands apart so a log names the last one before a firmware death, `0x40` **no scan at all** (the device is created, the level holds and tears down: it tells a death at the device from one at the scan), `0x80` **the receive buffers** (the data interface, its version, the six receive rings, and a second scan after them: the first scan is the A/B control), `0x100` the scan's diagnostics (channel-visit counters, the radio's statistics after each round, event counts), `0x200` the receive buffers return to software ring 1 instead of the firmware's own manager. `0` is the 2026-11-12 boot that asserted the firmware; `63` the 02:05 boot that scanned and kept no frame |
| `piano-wifi-l6regwait=MS`, `piano-wifi-l6pace=MS`, `piano-wifi-l6hold=MS` | 2000, 50, 8000 | how long level 6 waits for the regulatory event, how far apart it paces the radio's set-up, and how long `0x40` holds after the virtual device |
| `piano-wifi-l7=N` | `0x3f` = 63 (a decimal value is always safe to write) | level 7's pieces (§13.3, `WifiSta.h` `kStaFlag*`): `0x01` the station's parameters after the virtual device (802.3 out and in, the stream count, three power-save parameters, power save off), `0x02` the peer's data set-up (its default routing and one reorder queue per traffic identifier 0-16), `0x04` wait for the data interface's peer map, `0x08` a 5 GHz access point before a 2.4 GHz one (unless it is weaker than -82 dBm), `0x10` a radar channel may be chosen (joined passively), `0x20` a deauthentication of ours at the teardown, `0x40` native Wi-Fi frames instead of 802.3 (the A/B of `0x01`'s format), `0x80` **a dry run**: the network is chosen and logged, nothing is created; since boot 10a also `0x100` the radio's transmit power limits with `0x01`'s parameters, `0x200` level 7's own scans begin with directed probes naming the profile's network (a network that hides its name), `0x400` an access point that does not answer or refuses is taken down and the next candidate tried. Default since boot 10a: `0x73f` = 1855 |
| `piano-wifi-l7wait=MS`, `piano-wifi-l7hold=MS` | 600000, 30000 | how long level 7 waits for the join profile after the scan (a RAM probe's profile is written over the USB shell after the boot), and how long an association is watched before the teardown (`0`: until a suspend; the level then has no hard deadline) |
| `piano-wifi-l7file=0`, `piano-wifi-l7cands=N` | 1, 4 | `0`: the join profile only through the sysctl (by default the kext also reads the root-only file `/private/var/db/piano-wifi/profile` itself every 2 s while it waits, with the helper's checks); how many access points of the profile's name are tried in turn (at most 6) |
| `piano-wifi-l7tries=N`, `piano-wifi-l7frame=MS`, `piano-wifi-l7step=MS`, `piano-wifi-l7scans=N` | 3, 500, 3000, 3 | the tries of the authentication and of the association request, the wait for each answer (the association's is twice it), the wait for a command's answer (the start response, the peer map, the confirmation, the teardown's), and how many scans of its own level 7 runs when the network is not in the list |

## 7. Build, kernel collection

`tooling/k27/build-native-piano-wifi.sh` (host tests `package/PianoWifi/tests/wifi_host.cpp`, `tests/bringup_host.cpp`,
`tests/qmi_host.cpp` and `tests/wmi_host.cpp`, the `FatVolume.c` drift check, the objects in `out/native-kexts/PianoWifi`, the
import check), descriptor `tooling/k27/kext/PianoWifi.json` (`com.unixnu.driver.PianoWifi`), listed in
`configs/k27-native-kexts.txt`, so `tooling/k27/build-native-image.sh` links it into every native KC (inert). The descriptor names
two libraries, which is why PianoWifi comes after them in the list: `PianoA830` for the two lock words
(`sm8750_rpmh_drv2_owner`, `sm8750_gcc_vote_owner`) and `NativeUfs` for `native_ufs_part_find` / `native_ufs_part_read` (level 4's
firmware read), and from 2026-11-12 also the two words of the shared rails helper (`piano_wcn_rails_state`,
`piano_wcn_pmu_owner`, which PianoA830 defines: `drivers/bluetooth.md` §7). The six symbols the import check reports are
exactly those ("6 from the libraries … 0 unresolved"); the KC link resolves them. The build script also runs
`package/include/tests/run.sh`, the helper's own host tests.

The macOS side is a second kext of the same package: `tooling/k27/build-native-piano-wifi-ethernet.sh` builds
`package/PianoWifi/PianoWifiEthernet.cpp` against `IONetworkingFamily` (and the k27 tree's networking headers, as
PianoNullEthernet is built), descriptor `tooling/k27/kext/PianoWifiEthernet.json` with the same vtable check PianoNullEthernet
has. It owns no hardware: its personality matches the **resource** `PianoWifiLink`, which PianoWifi publishes only when level 6
came up, so without `piano-wifi=6` macOS never sees `en1`. Both are in the kext list, after PianoWifi.

The link: `23 native kexts linked, none from outside this repository`; PianoWifi 604 pointers, PianoWifiEthernet 442 pointers,
`vtable check PianoWifiEthernet vs IOEthernetController: 0 problem slots`; `tooling/k27/check-native-kexts.py` passes on both
descriptors and on the collection's log.

| File | What |
|---|---|
| `WifiHw.h` | the register facts of all four levels, each with its source |
| `WifiSurvey.h` | level 1: what is read and the gates that keep it off unpowered blocks (no store in the file) |
| `WifiPhySeq.h` | the 128-write QMP v7 PHY sequence (PaganiWifi's, from the 13T's vendor device tree) with its provenance note |
| `WifiBringup.h` | levels 2 and 3 as one portable sequence over callbacks: the writes, their order, read-backs, waits and timeouts |
| `WifiMhi.h` | level 4: the firmware file's layout, the DMA arena, the BHIe vector table, the BHI/BHIe boot state machine, the MHI contexts |
| `WifiChannels.h` | level 5's transport: the two IPC router channels' contexts and rings, the start commands, the receive buffers, a send, the event walk |
| `WifiQmi.h` | level 5: the QMI message writer and the typed readers (on the copied codec's parse) |
| `WifiWlfw.h` | level 5: the firmware's message set and the step machine, the memory answer, the downloads, the copy-engine configuration, the shadow list |
| `WifiSmmuWindow.h` | level 5: the 32-bit window for stream 0x1401 — a translating context bank and its two-page table (§10.11) |
| `WifiQrtr.c/.h` | the IPC router and the QMI header/TLV parse: a copy of PianoPower's with two additions (`UPSTREAM`) |
| `WifiFirmware.c/.h` | the firmware read in place from `modem_a` through NativeUfs (ranges only; no file is ever held whole) |
| `WifiCe.h` | level 6: the copy engine — the three rings of a pipe in the window, the registers that describe them, a send, the receive buffers, the event walk and the teardown |
| `WifiHtc.h` | level 6: the host transport — the header, the ready message, a service connected, set-up complete, the trailer's credit records |
| `WifiWmi.h` | level 6: the firmware's message interface — the codec, the initialisation command with this chip's resource configuration, the capability and ready events, a virtual device, the scan and its events, the management-frame parse and the scan list |
| `WifiLink.h` | level 6: the ladder over those three, as one step machine with a clock and one outstanding command |
| `PianoWifiEthernet.cpp` | the macOS side: the `IOEthernetController` publishing `en1` link down with the scan list (§13.1), its own kext, gated on the resource `PianoWifiLink` |
| `FatVolume.c/.h`, `SmmuStream.h`, `SpmiObserver.h` | copies of PianoPower's and PianoDwc3's (`UPSTREAM`; those packages are not modified) |
| `PianoWifi.cpp` | the IOService: the windows, the logged writes, the RPMh transactions, the SPMI reads, the DMA window, BAR0, the chip's register window, the levels' reports, the scan's registry properties and the power contract's suspend hook |

## 8. Bring-up ladder, test plan and effort

RAM boots only, cable to the Mac, owner present, `piano-reset-after` as the backstop. Each level adds writes only after the previous
level's log was read. Effort = agent work + owner boots, assuming the PaganiWifi values hold for piano.

| Level | What | Pass | Effort |
|---|---|---|---|
| 1 (built) | survey: GCC/TCSR/TLMM, PARF/DBI/iATU and PHY only behind their gates, SMMU 0x1400/0x1401, cmd-db; with `-spmi=1` the eight regulators, BT_EN, sleep clock | the log below; nothing written | done; 1 boot |
| 2 (built) power | the six PMU rails **through the shared helper** (`package/include/PianoWcnRails.h`: the PMU lock, then a read-back when Bluetooth holds them already, else the votes — mode NPM, the voltage piano's DT asks for, enable; DRV-2 lock), then this level's own steps inside the same lock, PERST# low, CLKREQ# to its function, WLAN_EN low 150 ms then high + 50 ms, SW_CTRL read. No sleep clock pin is driven (§10.3) | the rails read back enabled at the voted voltages, the UFS PHY's supplies unchanged | done; 1–2 boots |
| 3 (built) link | GDSC release (GCC vote lock), RCGs, branch votes, TCSR clkref, BCR pulses, PARF set-up, the v7 PHY sequence, PHYSTATUS, BDF-to-SID, root port + memory window, PERST# high ≥ 100 ms later, LTSSM, L0 (Gen2 cap), iATU 0 and 1, config read, BAR0, SMMU stream 0x1401 | `17cb:110e` from the endpoint, BAR0 placed | done; 3–5 boots |
| 4 (built) MHI firmware | the DMA window (24 MiB, 36-bit, write-combined), BAR0's MHI block, `image/peach/amss20.bin` read at run time from `modem_a`, BHI (the boot loader), the MHI contexts, BHIe (the second ELF), mission mode; polled, no MSI | EE mission mode (AMSS) | done; 2–4 boots |
| 5 (built) QMI | the MHI channels 20/21 and their events, QRTR, and the WLFW service: the radio's and the chip's capability, the indication registration, the host capability, the firmware's memory regions out of the window, regdb and the China-region board file in 6 KiB segments, the PHY and auxiliary microcodes by address, the trace configuration; `piano-wifi-mode=1` also the options, the copy-engine configuration and the mode | the firmware's "ready" indication | done; 2–4 boots |
| 6 (built) the pipe and the scan | the copy engine's rings (four pipes), the host transport (ready, the message interface connected, set-up complete), the service-ready and ready events, the host's initialisation, a station virtual device, a **passive scan** and its results; the macOS side's skeleton (`en1` link down with the list) | the firmware's ready event, then the scan's networks (§8.6) | done; 2–4 boots |
| 7 join | association: the regulatory answer read, the channel list sent, the virtual device started, a peer, the four-way handshake in a user-space supplicant, the keys installed | a link to the owner's access point | fresh 4–8 weeks; Ath12kWifi + peach deltas 2–4 weeks |
| 8 traffic | the data path: the reorder, transmit and buffer-manager rings, the firmware's data interface (HTT), frames in and out, `en1` up and routed | a DHCP lease and Safari loading a page | fresh 4–8 weeks |

Totals: about 2–3 months to a working station with Ath12kWifi for levels 7 and 8 (20–30 owner boots); 6–9 months fully
licence-clean. Levels 1–6 are written; what is left is association, the data path, and the owner boots of §8.8.
The numbering of the levels above 4 shifted twice. First when levels 4 (BAR/MHI) and 5 (firmware) of the first plan became one
level: the firmware load needs the BAR and the contexts in the same run, and splitting them meant two boots that each proved
nothing. Then when the transport, the message interface and the scan became one level 6: the scan is the only proof that the
transport and the initialisation were right, and the macOS side needs something to publish. What was 7 (scan), 8 (join) and 9
(macOS) is now part of 6, 7 and 8.

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
PianoWifi: wcn rails (ours 0x57494649): the six rails voted and read back (6 with a voltage): state 0x80003f01
   (or, when PianoBluetooth already has them: "the six rails were up and read back enabled: owners now 0x80003f03")
PianoWifi: step 201: the rails as the PMIC has them (SPMI observer reads)
PianoWifi: step 202: the six PMU rails by RPMh: mode NPM, voltage, enable (DRV-2 lock per transaction)
PianoWifi: rsc: ldof3 (0x.....): 3 command(s) through TCS 2 completed after N us (status 0x10101 0x10101 0x10101); the DRV-2 lock held N us, taken after 0 waits
   (six such lines, in the order ldof3, ldof2, smpf5, smpd4, smpi7, smpg3; the hold is the TCS's own completion time, expected
   well under a millisecond (not yet measured): a hold near 100 ms or above is the P17 hazard of §10.22 coming back)
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
sequence, 256 cleared BDF-to-SID words plus one entry, the root port's bus numbers / memory window / its unused I/O and prefetchable
windows closed (six words, from the fourth round) / command / link control 2, one
PERST# release, the LTSSM enable, two iATU regions, the endpoint's BAR0 and command, and one SMMU group + bank. Before all of it,
one RPMh command: the PCIe NoC's bus clock manager `SN4` voted at the link's peak (§10.21; `piano-wifi-noc=0` skips it).

```
PianoWifi: cmd-db: SN4 (the PCIe NoC's bus clock manager) 0x50020: unit N, width W, clock domain D      (level 1's stage 5)
PianoWifi: options: noc 1 (the PCIe NoC's bus clock manager voted before level 3's clocks)
PianoWifi: options: socreset 1 (the chip's power-on reset before BHI), msi 1 (MSI into a sink page), ctxearly 0 (MHI contexts before the doorbell), ras 1 (TLP counters)   (level 4 and up)
PianoWifi: rsc: SN4 (0x50020): 1 command(s) through TCS 2 completed after N us (status 0x10101 0 0); the DRV-2 lock held N us, taken after 0 waits
PianoWifi: PCIe NoC: SN4 voted: peak 1000000 kB/s (Gen2 x2) -> vote V (unit N, width W, bus width 8), command 6000VVVV: taken (0)
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
PianoWifi: root port: windows as found: I/O .... upper ........, prefetchable ........ upper ........:........; closed: I/O 00f0, prefetchable 0000fff0
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
`PCIe NoC: SN4 ... NOT taken` or `SN4 is not in cmd-db` does not stop level 3 (configuration and BAR accesses go the other way,
CPU to chip, through keep-alive managers), but the chip's DMA at level 4 then most likely never completes (§10.21).

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
PianoWifi: level 4: BHI identity at the start: MSM hardware ID N of 6 words non-zero, OEM public key hash N of 24 non-zero (N all ones), digest ........
PianoWifi: firmware partition modem_a (LUN 4, NNNNN blocks of 4 KiB)
PianoWifi: firmware: image/peach/amss20.bin on modem_a, 6930496 bytes: boot loader 524288 bytes, a second ELF image 6406208 bytes at 524288
PianoWifi: firmware: in the window: boot loader at 0x........, 13 segments, vector table at 0x........ (13 entries)
PianoWifi: firmware: in the window: boot loader sha256 c0e4bb91... (the stock file's first 512 KiB), the file as the window holds it, 6930496 bytes, sha256 02bbc028... (the stock amss20.bin Ubuntu runs)
PianoWifi: level 4: chip power-on reset: chip LTSSM ........ -> 00000111 (N writes), GCC hot reset ........ -> ........, interrupts cleared, QFPROM power ........ -> ........ (VDD4BLOW was on, now off | was off), BHIe vector words cleared, Q6 cookie ........
PianoWifi: level 4: chip power-on reset: warm entry ........ -> 00000000, reset cause ........, global reset ........ -> ........, MHISTATUS ........ -> ........ after MHICTRL's reset, EE ........; 7 receive vector words cleared
PianoWifi: level 4: BHI identity after the chip's reset: ...                  (the same digest as at the start: the boot loader's fuse read did not change)
PianoWifi: level 4: crash dump: 9 segments of 512 KiB from bus 0x20......, the vector table at 0x20...... (144 bytes), given to the chip before the boot with session 0x........ (receive status 00000000)
PianoWifi: level 4: msi: cap 0x50 control .... -> .... (64-bit addresses), address 00000000:2070.000 data 4d53, command 00100406: one vector into the sink page
PianoWifi: level 4: msi: the sink page is at bus 0x2070....
PianoWifi: level 4: channels 20 and 21 planned before the boot (state disabled, burst mode disabled, the data event ring)      (levels 5 and up)
PianoWifi: level 4: contexts at 0x........ (channels 128, event rings 2: ...), given to the device at READY, as the MHI host does
PianoWifi: level 4: the image: 524288 bytes at bus 0x20000000 (the 32-bit window, translated by the SMMU), aligned to 512 KiB; the vector table at 0x20700000
PianoWifi: level 4: before the doorbell: MHI +00: ........ x8        (6 lines MHI, 3 lines BHI with +40 the serial number as --------, 4 lines BHIe)
PianoWifi: level 4: TLP counters: root port at 0x..., (11 of 11 on), endpoint at 0x... (N of 11 on)        (or "none": the capability is not there)
PianoWifi: log: from here until the ladder stops, this thread's lines are held with their time since now and printed by a helper thread at every phase change and within a second (piano-wifi-defer=0 prints them at once)
   ... the held lines then come out in batches (each phase change, or after a second), every one with its [+N ms]:
PianoWifi: level 4: boot begun in phase boot loader over BHI (EE PBL); BHI status 00000000 before, 00000000 after this host cleared it (the clear took) [+0 ms]
PianoWifi: level 4: after the doorbell +0 ms: MHISTATUS 00000000 (state 0x0), EE 00000000, BHI status 00000000, error code ........, BHIe status ........ [+0 ms]
PianoWifi: level 4: changed +N ms: MHISTATUS ... BHI status 80000000 ... [+N ms]     (one line per change, at most 48)
PianoWifi: level 4: waiting for READY after N ms (EE PBL, state 0x7) [+N ms]
PianoWifi: level 4: contexts needed after N ms (EE PBL, state 0x1) [+N ms]
PianoWifi: level 4: image over BHIe after N ms (EE SBL, state 0x2) [+N ms]
PianoWifi: level 4: mission mode after N ms (EE AMSS (mission mode), state 0x2) [+N ms]
PianoWifi: level 4: mission mode after N ms, M steps, 1 context writes, K event acknowledgements; EE AMSS (mission mode), ... [+N ms]
PianoWifi: level 4: device wake asserted (channel 127's doorbell = 1) [+N ms]
   ... and when the ladder stops:
PianoWifi: log: N line(s) held for M ms, K printed by the helper thread and L now
```

Every line held from the doorbell on carries `[+N ms]`, its time after the doorbell: the gaps between them are the host's own, not
the console's (wifi-4a's 2 s gap was the console's, §10.24). A `log: N later line(s) did not fit` line means more than 480 lines
were written (320 before the seventh round); the first 480 are the ones kept. A crash ends the holding: the held lines are printed first, then the `crash:`
lines as they are written, so the report of a death is never what a full buffer drops. A `watchdog: level N is … ms past its hard
deadline` line (from the helper, every 30 s) means the ladder's thread is stuck inside that level: the last held lines above it and
the stage in the line say where.

**What the first level 4 boot must show before level 5 is designed against it**: `EE PBL` at the start (the chip is in its primary
boot loader — `EE AMSS` already there would mean something else booted it), the firmware line with the 6930496-byte file split at
512 KiB (that is the peach shape: a boot loader and a second ELF), `boot loader over BHI` → `contexts needed` → `image over BHIe`
→ `mission mode`, and the tablet still alive afterwards.

Failure meanings: `the MHI register layout is not usable` — BAR0 does not carry the MHI block where sheng and PAGANI have it;
`no partition` — NativeUfs had not published `modem_a` yet (level 4 does not wait: rerun, or add `piano-wifi-delay=20`);
`BHI reported an error` with an error code and three debug words — the boot loader refused the image (its address alignment, or
the wrong file), or its read of it failed; debug words that are all 0 mean it refused before any transfer (sheng's page-aligned
address), debug words naming the image's address and size mean it had started one (wifi-3a/3b: `ef0e0003`, `20000000 00000000
00080000`, 45 ms after the doorbell, §10.23); since the 2026-11-12 fix an all-ones status counts as an error too, as in Ubuntu's MHI
host, unless it is the register's own power-on value that this host could not clear (`the clear did not take`, and it never
changed); `BHIe reported an error` — the vector table or a segment; `the device reported a system error` / `went to its crash
dump environment` — the firmware started and died, and the `crash:` lines that follow (§10.24's table) say why, from its own dump; `a phase timed out` — the device
stopped answering, which with `MHISTATUS ffffffff` means its PCIe core restarted under us (PAGANI re-applies BAR0 and the decode
bits there; this driver does not yet), or, with MHISTATUS and EE still answering and BHI's status unchanged since the doorbell
(now the 0 this host wrote), the boot loader never finished reading the image (how 2026-11-12's first four boots were read,
before the NoC vote and the clear: §10.21; the fourth round showed they were most likely the same refusal, which the old rule
mistook for an untouched register and waited out).

After any failure the level prints what the chip and the SoC say about the DMA, and those lines decide the next step:

```
PianoWifi: level 4: failed in phase boot loader over BHI: BHI error code ........, debug ........ ........ ........; BHI status 00000000 before the clear, 00000000 after (it took); N register change line(s)
PianoWifi: level 4: at the end +N ms: MHISTATUS ... EE ... BHI status ... error code ... BHIe status ...
PianoWifi: level 4: at the end: MHI / BHI / BHIe ...                               (the same 13 lines as before the doorbell)
PianoWifi: dma: endpoint command/status 00100006, device control/status ........; AER uncorrectable ........ correctable ........
PianoWifi: dma: root port command/status ........, secondary status ...., device control/status ........, BAR0 ........ BAR1 ........; AER uncorrectable ........ correctable ........; PARF interrupt status ........
PianoWifi: dma: SMMU sCR0 ........, global fault sGFSR ........ sGFSYNR0 ........ sGFSYNR1 ........ (stream 0x....) sGFSYNR2 ........ sGFAR ........:........
PianoWifi: dma: SMMU bank 81 (the 32-bit window): sctlr 00000027 fsr ........ far ........:........ fsynr0 ........
PianoWifi: dma: TLPs since the doorbell, root port: tx MRd N tx MWr N tx CplD N tx Cpl N rx MRd N rx MWr N rx CplD N rx Cpl N UR N CA N CTO N   (- for a counter that would not switch on)
PianoWifi: dma: TLPs since the doorbell, endpoint: ...
PianoWifi: dma: the chip asked for data N time(s); the root port answered with N completion(s) with data and N without (...)   (or: "received no read request from the chip")
PianoWifi: dma: msi sink at the end: untouched (...) | N word(s) changed, the first at +0x.. = 00004d53 (an MSI): the chip's writes reach the window
PianoWifi: firmware: after the failure: the boot loader in the window is still the stock file's first 512 KiB (sha256 c0e4bb91...)
PianoWifi: level 4: BHI identity at the end: ...
```

The fourth round's lines (§10.23) say which of the two remaining explanations holds, and what to change next:

| What the lines show | Meaning | Next |
|---|---|---|
| root port `rx MRd 0` (and the endpoint's `tx MRd 0`) | the boot loader refused before it fetched a byte: its parameters or the chip's state | the address and size are Linux's; the state is what `socreset`, `msi`, `ctxearly` change: their A/B decides |
| `rx MRd` about 1024 (512 KiB in 512-byte reads), `tx CplD` 4096 or more, `tx Cpl 0`, the image `the stock …` | the chip got the right bytes and refused them: its signature check against its fuses | the BHI identity line (an OEM key hash that is zero, all ones or changes means the boot loader could not read its fuses) and the QFPROM line (VDD4BLOW) |
| `rx MRd` > 0 with `tx Cpl` > 0, or `UR`/`CA` > 0 | the root complex or the SMMU refused the reads with an error completion | bank 81's `fsr`/`far` and `sGFSR`: the stream or the translation |
| `rx MRd` > 0 and `tx CplD 0`, no `tx Cpl` | the reads went in and nothing came back (AXI/NoC/TBU) | the SN4 line, the TBU clock (`ddrss_sf_qtb`), the AXI master halt bit |
| the image `NOT the stock …` | the window does not hold the file (the UFS read into the write-combined window) | the copy path, not the chip |
| sink changed with `4d53` | the chip's writes reach the window through bank 81 (or the bypass): the path works both ways | — |

How to read them: an endpoint status with bit 29 (received master abort) or 28 (received target abort), or AER uncorrectable bit
20 (unsupported request) or 15 (completer abort) — the root complex or the SMMU refused the chip's read; a non-zero `sGFSR` with
bit 1 (an unidentified stream) — the chip's requests carry another stream than 0x1401 (`sGFSYNR1`'s stream says which: then the
BDF-to-SID entry of step 306 is not what the controller uses); a bank `fsr` with a fault bit and `far` inside or outside
`0x20000000` + the window — a translation fault at that address; AER bit 14 (completion timeout) in the endpoint — its read went
out and nothing came back (the NoC again, or the TBU); every word clean and BHI's status still 0 — the boot loader never issued
the read at all, which is what `piano-wifi-socreset=1` is the next test of (§10.21).

### 8.5 Level 5 (the firmware's QMI service): one RAM boot

`PIANO_EXTRA_ARGS="piano-usb=2 piano-wifi=5"` (`--out out/ram-wifi-l5`), everything else as §8.2. Add `piano-wifi-iova=1` to give
the chip 32-bit addresses instead of physical ones (§10.11), `piano-wifi-mode=1` to let the level send the last three messages that
start the radio, and `piano-wifi-shadow=1` to put the shadow register list in the configuration.

What it writes, beyond level 4's: in the DMA window the two channels' rings, 67 buffers of 8 KiB, the firmware's memory regions and
the two microcodes; in the channel contexts level 4 already gave the chip, channels 20 and 21 (state enabled, burst mode disabled,
the event ring, the ring); on the command ring two "start channel" commands; and one doorbell per submission (the command ring's,
the two channels', the event rings'). With `piano-wifi-iova=1` also one SMMU context bank, its two-page table (a buffer of its own,
outside the window the chip can reach) and the one store that points stream 0x1401 at it. The window is 64 MiB by default at this
level: `piano-wifi-dma=M` changes it.

```
PianoWifi: level 5: channels 20 and 21 planned (64 elements each, 536 KiB of buffers), channel doorbells at 0x..., event doorbells at 0x...
PianoWifi: level 5: the chip reports environment AMSS (mission mode)        (the control ring's events since level 4's last acknowledgement)
PianoWifi: level 5: both channels run (states 0x2 / 0x2) N ms after level 5 began, 63 receive buffers posted (channel 21's ring written to element 63, 1 doorbell(s))
PianoWifi: level 5: router: the chip said hello first and was answered      (or: "no hello from the chip within 500 ms: ours went first")
PianoWifi: level 5: router: our answer to the chip's hello completed on channel 20 N ms after the router began (code 0x2, 1 packet(s) out, 1 completed)
PianoWifi: level 5: the router is up (our node 1 port 0x4000, the chip's node N; the firmware's service 69 instance 0x101 on port P)
PianoWifi: step 502: the radio's capability
PianoWifi: step 502: answer to 0x0057 (transaction 1) after N ms: result 0 error 0 (accepted)     (one such line per answer)
PianoWifi: step 503: registering for the firmware's indications
PianoWifi: step 504: this host's capability
PianoWifi: step 505: waiting for the firmware's memory request
PianoWifi: step 505: indication 0x0035: the firmware asks for memory                              (one such line per indication)
PianoWifi: step 506: answering the memory request
PianoWifi: step 507: waiting for the firmware's memory to be ready
PianoWifi: step 508: the chip's capability
PianoWifi: step 509: the regulatory database
PianoWifi: step 509: board identifiers: the chip's capability 0xff, the radio's capability 0xff; the downloads carry 0xff
PianoWifi: step 509: the regulatory database (kind 4): regdb_xiaomi.bin, 24278 bytes, 4 segment(s) of at most 6144, file identifier 0xff
PianoWifi: step 509: answer to 0x0025 (transaction 6, regdb_xiaomi.bin segment 1 of 4, 6144 bytes) after N ms: result 0 error 0 (accepted)
  ... segments 2 and 3 ...
PianoWifi: step 509: answer to 0x0025 (transaction 9, regdb_xiaomi.bin segment 4 of 4, 5846 bytes, the last) after N ms: result 0 error 0 (accepted)
PianoWifi: step 510: the board file
PianoWifi: step 510: board file bd_p81.elf for board 0xff: the chip names no board (0xff: its OTP carries no board identifier), so the stock API-1 default board file, ...
PianoWifi: step 510: the board file (kind 1, at most 256 KiB sent): bd_p81.elf, 125272 bytes, 21 segment(s) of at most 6144, file identifier 0xff
  ... 21 answer lines ...
PianoWifi: step 511: the PHY microcode
PianoWifi: step 511: phy_ucode20.elf, 290816 bytes, by address
PianoWifi: step 512: the auxiliary microcode
PianoWifi: step 512: aux_ucode20.elf, 61440 bytes, by address
PianoWifi: step 513: the trace configuration
PianoWifi: step 513: the trace configuration: qdss_trace_config_v2.cfg, 6241 bytes, 2 segment(s) of at most 6144, file identifier 0xff
PianoWifi: step 514: waiting for the firmware to be ready
PianoWifi: step 514: indication 0x0038: the firmware is ready
PianoWifi: level 5: the chip: chip 0x..... family 0x..... system-on-chip 0x....., board 0x.., 1 radio(s)
PianoWifi: level 5: board identifiers: the chip's capability 0xff, the radio's capability 0xff (only noted); board file bd_p81.elf
PianoWifi: level 5: the firmware: version 0x........ built ................, capabilities 0x........, status 0x...., 1 MAC(s), .... mV
PianoWifi: level 5: memory: 1 request(s), N region(s) of .... KiB asked for, .... KiB given
PianoWifi: level 5: region 0: .... KiB of host memory at 0x........      (one line per region, at most eight)
PianoWifi: level 5: N requests, N responses, 3 indications, M download segments, 0 shadow registers, 0 unexpected messages
PianoWifi: level 5: the transport: N packets out, N in, N completions, 2 commands, 0 events dropped, 0 refused claims; the router: N packets in, N data, 0 unrouted, 0 refused sends
PianoWifi: level 5: flow control: N data packet(s) asked for an answer, N RESUME_TX sent (from our port 0x4000 to the sender, naming node 1 port 0x4000), none owed
PianoWifi: level 5: the firmware is ready after NNNN ms (step 518: done)
PianoWifi: level 5: MHI after mission mode: N M0 event(s) answered by ringing every doorbell, 0 M1 answered with M2
PianoWifi: level 5: at the firmware's ready: channel 20 state 0x2, the chip's read pointer at element N, ours N, written to N (0 owed), N completion(s) (last code 0x2), N doorbell(s); channel 21 state 0x2, ... (63 buffers posted), N completion(s) (last code 0x2, N bytes), N doorbell(s); data event ring: the chip at element N, this host at N; 0 event(s) dropped, 0 element(s) given back unread, 0 error code(s), 0 doorbell-mode event(s)
PianoWifi: level 5: the firmware is ready; WMI, the scan and the station are levels 6 and up (docs/drivers/wifi.md section 13)
```

The ring line (also printed where level 5 stops) is the transport's account: the chip's read pointer on channel 20 at ours means
it took everything sent; on channel 21 it is where the chip will write next (the posted buffers run from ours to the written-to
element); the data event ring's two positions equal means every event was walked. `0 receive buffers posted` cannot appear any
more without the level stopping at stage 501 right there (`channel 21 runs but has no receive buffer`).

With `piano-wifi-mode=1` three more steps appear after the ready indication (515 the firmware's options, 516 the host's
configuration, 517 the mode) and the last line says `step 518: done` all the same; with `piano-wifi-shadow=1` the count in the
"N shadow registers" line is 40. With `piano-wifi-iova=1` two lines come before level 4's firmware line:

```
PianoWifi: iova: window 0x20000000 + 64 MiB -> 0x......... in 32 blocks of 2 MiB, table at 0x.........; SMMU groups 128 banks 83 pages 128: free slot N bank M (output 40 bits)
PianoWifi: iova: stream 0x1401 translates through bank M (smr 80001401 s2cr 000000NN sctlr 00000425)
```

**What the first level 5 boot must show before level 6 is designed against it**: the two channels running (`states 0x2 / 0x2`), the
router line with the firmware's service announced (service 69 instance 0x101: without it nothing else can happen, and the level
stops there), the memory request answered with regions inside the window, the four files read from `modem_a` with no error, the
`3 indications` of the normal path (the memory request, the memory ready, the ready), `0 unexpected messages`, `0 events dropped`
and the ready line with step 518. The chip's own identities (chip, family, board, the firmware's version and build time) are what
level 6 needs in order to know which board file and which register map it is talking to; note the board identifier, since it is the
file identifier every download carries.

Failure meanings: `the DMA window has no room for the channels' rings and buffers` — `piano-wifi-dma` is too small (64 MiB is the
default at this level). At stage 502 the ring line and the `the chip announced service …` lines come first: a hello that arrived
with no service 69 means the firmware's WLAN service did not start; service 69 under another instance word than `0x101` is taken
anyway (the router line then says so), and the word it had belongs in §12; channel 21's read pointer still at ours with nothing
received means the chip never wrote to the buffers (the ring or the doorbell is not what it reads); `our hello was sent and never
completed` means the chip never took channel 20's element. `the chip's doorbell arrays are not where the register space says` —
the chip reports a channel or event doorbell outside BAR0's MHI block, which would mean level 4 read a layout this driver does not
know. `a channel did not start` —
no command completion within 2 s, or the chip refused it (the completion code is in the line): the firmware is in mission mode but
its router is not, which on this chip family has meant the image did not finish its own start-up. `the firmware's QMI service was
not announced on the router` — the hello, the packet counters and the number of services are in the line before it: no hello means
the chip is not speaking the router's protocol on those channels at all; a hello with no announcement means its WLAN service did
not come up (its own log would say why, and level 5 has no way to read it). Then, per step: `the firmware refused a request` with
the result and error — the firmware did not like what it was told (the board file is the usual suspect: the wrong region's file, or
a file identifier that is not the board identifier it reported); `a firmware file could not be read` — the file is not in
`image/peach/` of `modem_a` (the names are in §2), or `NativeUfs` stopped answering; `the window has no room for what the firmware
asked for` — a region this driver must answer did not fit and was too small for the "ask again in smaller pieces" fallback (the
memory line says how much was asked for); `a step timed out` — the firmware stopped answering: the `unanswered: request …` line
names the request (and the segment), the transport counters say whether anything came back at all, and the `flow control:` line
whether the firmware's router was left waiting for us (P21, §10.26: `one still owed`, or a data count stuck at a multiple of ten
with fewer answers sent than asked for, is this host's fault, not the firmware's); with a live chip the `dma:` lines that follow
(TLP counters, AER, the SMMU's global and window bank fault registers, the MSI sink) say whether its DMA left the window or a
request of it was refused on the link; `the chip reported a system error or its crash dump environment` (or, before that, `the chip
died while channel N was being started`) — the firmware started and died, the step's name says where, and the `crash:` lines
after it say why: the dump the firmware wrote into the segments level 4 gave it, decoded (§10.24's table). Each answer's own line
carries the firmware's result and error codes: `(refused)` with an error is the firmware saying no to that message (for a
download, to that segment). The trace configuration (step 513) is the one step whose refusal or silence does not end the level:
`the trace configuration was refused` / `was not answered` / `is not on the partition`, then the wait for the ready indication.

### 8.6 Level 6 (the copy engine, the transport and a passive scan): one RAM boot

`PIANO_EXTRA_ARGS="piano-usb=2 piano-wifi=6 piano-wifi-iova=1"` (`--out out/ram-wifi-l6`), everything else as §8.2.
`piano-wifi=6` switches `piano-wifi-mode` on by itself (the firmware's radio has to be running), so this boot does everything
boot 6 of §8.8 did but the shadow list, and then level 6. Useful variations: `piano-wifi-band=0` (the 2.4 GHz band only: a shorter scan that needs no
regulatory answer for the upper band), `piano-wifi-scanwait=20000` (a slow scan), `piano-wifi-bssid=1` (whole addresses in the
log — **not** for a document).

What it writes, beyond level 5's: in the DMA window the four pipes' rings (a source ring of 16 elements for pipe 0, one of 32 for
pipe 3, a destination ring and a destination-status ring of 32 for pipe 1 and of 64 for pipe 2) and their buffers (92 of 2 KiB),
one word per ring for the chip's own pointer, and eight send buffers of 2 KiB; in the chip's register space (through BAR0's
window register) each ring's base, size, descriptor size, pointer address and enable bit, the largest buffer of each destination
ring, and then one pointer write per submission; and on the wire the transport's two control messages, the host's initialisation,
a station virtual device, the scan and the two teardown commands.

```
PianoWifi: level 6: 4 pipes: 0, 1, 2 and 3; 92 receive buffers posted, NNN register writes
PianoWifi: step 601: waiting for the transport's ready
PianoWifi: step 602: connecting the firmware's message interface
PianoWifi: step 603: the transport's set-up complete
PianoWifi: step 604: waiting for the chip's capability
PianoWifi: step 605: the host's initialisation
PianoWifi: step 606: waiting for the firmware to be ready
PianoWifi: step 607: a station virtual device
PianoWifi: step 608: a passive scan
PianoWifi: step 609: the scan's results
PianoWifi: step 610: the teardown
PianoWifi: step 611: done
PianoWifi: level 6: the transport: N credits of M bytes, 8 endpoints, version 0x...; our endpoint 2 (pipes 3 out, 2 in), K credit reports (K credits), 3 control messages
PianoWifi: level 6: the chip: 2 radio chain(s), board 0x.., high-throughput 0x..... very-high-throughput 0x....., system 0x...., NN scan channel(s), 1 radio(s), chain mask 0x3, 0 memory request(s)
PianoWifi: level 6: the firmware is ready: status 0x0, NN peer(s), 0 extra address(es); the chip's own address is all zeros (this chip's firmware reports none: the unit's own is in the persist partition, section 13.1)
PianoWifi: level 6: the scan: 22 channel(s) asked for, started 1, N scan event(s), M channel visit(s), completed
PianoWifi: level 6: network 0: ssid "..." bssid xx:xx:xx:**:**:** channel 6 (2437 MHz) signal -NN dBm, N beacon(s) 0 probe(s)
   (one line per network, at most 32; the identifier is printed, the address masked unless piano-wifi-bssid=1)
PianoWifi: level 6: N commands, M events (2 extended capability), K management frames (0 refused by the firmware's own checks, 0 unreadable), 0 unexpected messages, 0 messages dropped, 0 credit waits; the copy engine: N out (B bytes), M in (B bytes), N completions, 0 refused claims, 0 dropped
PianoWifi: level 6: the scan is done after NNNN ms (step 611: done)
PianoWifi: level 6: the teardown: the scan ended by itself, the virtual device deleted, the rings disabled
PianoWifi: level 6: 24 writes (6 kinds, 0 refused accesses in all); repeated: ...
PianoWifi: level 6: the scan is done; association, keys and the data path are levels 7 and 8 (docs/drivers/wifi.md section 13)
PianoWifi: level 6: the resource PianoWifiLink is published: PianoWifiEthernet's en1 may come up (link down, the scan list in the property piano-wifi-scan; tooling/native/piano-wifi prints it)
PianoWifiEthernet: en1 published (built in, link down: the station of levels 7 and 8 is what brings it up); the scan list is the property piano-wifi-scan (tooling/native/piano-wifi prints it)
```

**What the first level 6 boot must show before level 7 is designed against it**: the transport's ready with a credit count and a
credit size that are not zero (without them nothing can be sent, and the level stops at step 601); `our endpoint 2 (pipes 3 out,
2 in)`, which is the proof that the service tables level 5 sent are the ones the firmware uses; `0 memory request(s)` in the
capability line (a firmware that does ask for memory ends the level: §10.16); the firmware's ready event; `started 1` with the
scan's events; **at least one network** in the scan lines (the owner knows which ones should be there, which is the real
measurement of this level); `0 unexpected messages`, `0 dropped` and `0 refused claims`; the teardown line; and `en1` in
`ifconfig` with `status: inactive`, which `tooling/native/piano-wifi status` prints as well.

On the tablet afterwards: `tooling/native/piano-wifi` prints the table (addresses masked), `piano-wifi --full` the whole
addresses, `piano-wifi status` what the ladder reached and whether `en1` is there. Nothing of that list belongs in a commit
(§11).

Failure meanings: `the DMA window has no room for the copy engine's rings and buffers` — `piano-wifi-dma` is too small (the
default at this level is level 5's 64 MiB, of which level 6 needs about 200 KiB). `a step timed out` at 601 — the firmware never
sent the transport's ready: the rings are not where the chip reads them (the pointer words and the register writes are in the
line above), its radio was not started (`piano-wifi-mode=0`), or the pointer writes have to go through the shadow registers
after all (§10.19). At 602 with `the firmware refused the service` — the message
interface is not service `0x100` on this build. At 604 — the host's set-up complete was not accepted, or the service-ready event
does not parse (`a message did not parse`). At 606 — the initialisation was refused: the resource configuration is the suspect,
and its 72 words are in `WifiHw.h` with the values of this chip's own entry. `the firmware asked for memory this level does not
hand out` at 604 — the firmware wants host memory chunks in the initialisation, which this level does not answer (§10.16). At
609 with `the scan did not run` — the firmware reported a start failure, and the reason is in the scan line. A scan that starts
and then stops answering is **not** a failure: what it found stands, the teardown runs, and the line says "after this driver
stopped waiting for it". `the copy engine refused the message` — a source ring was full with no completion coming back, which
means the chip is not reading the ring (compare the counters). A message the transport cannot read is **not** a failure: it is dropped and
counted in the level's line (`N messages dropped`), as this firmware's own host does, and the step's deadline is what decides
if they are all there is.

### 8.7 After any failure

Every level leaves the hardware at WLAN_EN low and PERST# asserted, rails still voted (a disable could cut Bluetooth's supply;
`piano-wifi-release=1` withdraws only the enables this driver added). The GCC votes and the SMMU entry are not withdrawn either:
they cost nothing and the next boot is a fresh SoC. Nothing retries in a loop, so a failed boot ends with the log and an idle
tablet, and `piano-reset-after=300` is the backstop.

Level 5 adds two things a failure leaves behind: the firmware has the addresses of its memory regions and may keep writing to them
(the window is never freed, §10.10), and with `piano-wifi-iova=1` the endpoint's stream keeps its translating bank and its table —
both for the rest of the session, since nothing but a reset guarantees the chip has stopped. The firmware's own channels are left
running: a failed step does not stop them, because stopping a channel the firmware is using is itself a write this driver has no
reason to be sure of.

Level 6 cleans up after itself as far as it can, which is more than level 5: the scan is stopped if it was running, the virtual
device is deleted, and only then are the copy engine's rings disabled — in that order, and only once the chip has taken the two
commands (a ring disabled with a descriptor the chip has not read loses that message). The window and its buffers stay where
they are, as at level 5. What a failed level 6 leaves running is the firmware's radio, which level 5's mode message started:
nothing here switches it off again, and a boot that fails above step 604 should not be left idle for long.

### 8.8 The batched tablet test list

One session, the owner present, cable to the Mac, RAM boots only, `piano-reset-after=300` as the backstop. Each boot is read before
the next is built: a level that fails is not retried at a higher level, and nothing is raised past the first failure.

| # | Boot args | Gate: what the log must show | If it fails |
|---|---|---|---|
| 1 | `piano-usb=2 piano-wifi=2` | the helper's `wcn rails … voted and read back` line, then every PMU rail `en 80` at or above the voted voltage; the two PHY LDOs unchanged; `WLAN_EN cfg ... io 00000002`; the log reaching `level 2: done`; the tablet alive afterwards (§8.2) | read §8.2's failure meanings; `SW_CTRL in 0` is not itself a failure but is the first suspect at boot 2 |
| 2 | `piano-usb=2 piano-wifi=3` | `PCS status1 ... (ready)`, `LTSSM 0x11 L0` with `debug1 ... (up)`, `id 110e17cb`, a BAR0 placed inside the window, `command ...6`, the SMMU line (§8.3) | §8.3; `piano-wifi-gen=1`, `piano-wifi-physeq=0` and `piano-wifi-stop=N` are the next boots' tools |
| 3 | `piano-usb=2 piano-wifi=4` | `EE PBL` at the start, the 6930496-byte file split at 512 KiB, `boot loader over BHI` → `contexts needed` → `image over BHIe` → `mission mode`, the tablet alive (§8.4) | §8.4; `piano-wifi-delay=20` when `modem_a` was not published yet. Failed four times on 2026-11-12 (§10.21), then in boots 3a and 3b below (§10.23): boots 4a and 4b below |
| 4 | `piano-usb=2 piano-wifi=5` | both channels running, the firmware's service announced, the memory request answered, the four files read, `3 indications`, `0 unexpected messages`, the ready line with step 518 (§8.5) | §8.5 |
| 5 | `piano-usb=2 piano-wifi=5 piano-wifi-iova=1` | the two `iova:` lines, then everything boot 4 showed: the chip works with 32-bit addresses, which level 6's rings need (§10.11) | if the `iova:` lines are there but a step then times out, the translating bank is the suspect: bank, table and fault registers are in the line, and boot 4's result stands as the comparison |
| 6 | `piano-usb=2 piano-wifi=5 piano-wifi-iova=1 piano-wifi-mode=1 piano-wifi-shadow=1` | the three extra steps (515, 516, 517), `40 shadow registers`, step 518, and the tablet alive for a minute afterwards with the firmware's radio running | a refusal at 516 means the copy-engine tables or the shadow list are not what this firmware wants (§12); at 517 it means the mode itself was refused |
| 7 | `piano-usb=2 piano-wifi=6 piano-wifi-iova=1 piano-wifi-band=0` | the transport's ready with credits that are not zero, `our endpoint 2 (pipes 3 out, 2 in)`, `0 memory request(s)`, the firmware's ready event, `started 1`, **at least one network the owner recognises** on the 2.4 GHz band, `0 unexpected messages`, `0 dropped`, step 611, the teardown line, and `en1` with `status: inactive` (§8.6) | §8.6's failure meanings. A timeout at 601 is the ring programming or the radio; a refusal at 606 is the resource configuration; a scan that starts and finds nothing is the regulatory domain (boot 8 reads it) |
| 8 | `piano-usb=2 piano-wifi=6 piano-wifi-iova=1` | the same, with 22 channels: the 5 GHz networks appear as well, and the channel-visit count is higher | if only the 2.4 GHz networks appear, the firmware's regulatory domain is not letting the upper band be scanned passively: level 7's first job is the regulatory event and the channel list (§13) |
| 9 | `piano-usb=2 piano-wifi=6 piano-wifi-iova=0` | the same as boot 7, with physical addresses instead of the window's: whether this firmware's message interface needs 32-bit addresses at all (§10.11) | a timeout at 601 here with boot 7 working is the answer: the rings must be in the 32-bit window, and `piano-wifi-iova=1` stays the default for level 7 |
| 10 | `piano-usb=2 piano-wifi=6 piano-wifi-iova=1 piano-sleep=1 piano-sleep-test=60` | after the scan: `suspend: stopping the radio`, `WLAN_EN is low and PERST# asserted`, the tablet asleep and awake again with the rest of the system intact, and `en1` still there (down) | the suspend hook is the only power state this driver has; a tablet that does not come back is a power bug of the suspend path, not of this driver (compare a boot with `piano-wifi=0`) |

Boots 1–3 are each a level's own gate and must be read in order; 4 is level 5's gate; 5 and 6 answer the two questions level 6
needed settled (32-bit addresses, and whether the firmware accepts this host's copy-engine description); 7 is level 6's own gate
and 8–10 are the three questions level 7 needs settled (the upper band, whether the 32-bit window is required, and that a
suspend leaves the tablet healthy). Every boot ends with WLAN_EN low and PERST# asserted on a failure, the rails still voted
(§8.7).

**The next two unattended boots (after §10.21's fixes; boot 3 of the table, which has now failed four times).** Both on the
queue's base `panic_on_crash_disabled=1 piano-usb=2 piano-usb-cold=0`, RAM boots with the queue's usual reset backstop:

| # | Boot args (beyond the base) | Gate | Then |
|---|---|---|---|
| 3a | `piano-wifi=4 piano-wifi-iova=1` | `cmd-db: SN4 ... unit N, width W`; `PCIe NoC: SN4 voted ... taken (0)`; `BHI status ffffffff before, 00000000 after this host cleared it (the clear took)`; a `changed` line with BHI status `80000000`, then `waiting for READY` → `contexts needed` → `image over BHIe` → `mission mode`; the GPU up as usual (P17's lock hold now in each `rsc:` line) | passes: the NoC vote was the cause (that boot is the evidence; `piano-wifi-noc=0` on a later boot would confirm it by failing again). Fails: the `dma:` lines say where the read went (§8.4's reading guide); a clean set with BHI's status still 0 points at the chip-side reset of 3b |
| 3b | `piano-wifi=6 piano-wifi-iova=1 piano-wifi-band=0 piano-wifi-socreset=1` | the `chip-side reset` line (global reset not `ffffffff`, the link still up), then everything 3a must show, then §8.8 boots 4–7's gates in one run (levels 5 and 6, a 2.4 GHz scan) | after a 3a that passed: the whole ladder on the Linux-like path; a failure at level 4 here with 3a passing means the reset itself hurts (keep `socreset=0`). After a 3a that failed: whether the reset is what the boot loader was missing |

3b is chosen to be useful whichever way 3a goes. If the owner prefers the ladder without a second variable, 3b without
`piano-wifi-socreset=1` is the alternative (it then only repeats 3a's question when 3a failed).

**Results (2026-11-12 12:31 and 12:37):** both failed at stage 401 the same way: SN4 taken, `BHI status 00000000 before`, then at
+45 ms the boot loader's own `ffffffff` / `ef0e0003` with debug `20000000 00000000 00080000`, every `dma:` word clean (3b's
partial reset: global reset `4 -> 4`, warm entry 0, cause 0). Neither the NoC vote nor that reset was the cause (§10.23).

**The next two unattended boots (after §10.23's changes).** Both on the queue's base `panic_on_crash_disabled=1 piano-usb=2
piano-usb-cold=0`, RAM boots with the queue's usual reset backstop:

| # | Boot args (beyond the base) | Gate | Then |
|---|---|---|---|
| 4a | `piano-wifi=6 piano-wifi-iova=1 piano-wifi-band=0` | the new defaults, i.e. Ubuntu's pre-boot path: the two `chip power-on reset` lines (`chip LTSSM … -> 00000111`, the QFPROM line, global reset not `ffffffff`), `msi: … one vector into the sink page`, `contexts … given to the device at READY`, `TLP counters: root port at …`, the firmware's `sha256 … (the stock amss20.bin Ubuntu runs)`; then a `changed` line with BHI status `80000000`, `waiting for READY` → `contexts needed` → `image over BHIe` → `mission mode`, and §8.8 boots 4–7's gates (levels 5 and 6, a 2.4 GHz scan) | passes: one of §10.23's five changes was the cause, and 4b's failure (the control) makes that evidence; the A/B that names it is one switch per later boot (`ctxearly=1`, then `socreset=0`, then `msi=0`, each on 4a's arguments). Fails at level 4: §8.4's table on the `dma:` lines; fails later: that level's own meanings (§8.5, §8.6) |
| 4b | `piano-wifi=4 piano-wifi-iova=1 piano-wifi-socreset=0 piano-wifi-msi=0 piano-wifi-ctxearly=1` | the control: 3a's pre-boot path (no chip reset, MSI off, the contexts before the doorbell) with the new measurements. Expected to fail as 3a did; what it must show is the `TLPs since the doorbell` lines, the `the chip asked for data …` / `received no read request` line, the sink line and the image's hash | read with §8.4's table: it answers, for the configuration that failed all day, whether the chip's DMA reached the buffer and whether the buffer held the stock file. If 4b passes, what is new in it is all that is left: the root port's windows closed (step 307) and the counters switched on (a write to each end's counter control), and a boot with `piano-wifi-ras=0` separates the two |

The two are chosen so that each result says something whichever way the other goes: 4a is the best chance of the boot loader
taking the image and runs the ladder as far as it goes; 4b is the measured control.

**Results (2026-11-12 13:57 and 14:03):** 4a passed level 4 — mission mode at 477 ms, the stock image in the window, every read
served — and the firmware was in its crash dump environment at the next register read, 2 s later (the console had held this
thread), so level 5 stopped at stage 501 with no answer to channel 20's start. 4b, the control, failed at stage 401 as 3a did, the
counters showing that the boot loader read the image (21520 reads served) before refusing it. P18 is cured by §10.23's pre-boot
path; P19 is §10.24.

**The next two unattended boots (after §10.24's changes).** Both on the queue's base `panic_on_crash_disabled=1 piano-usb=2
piano-usb-cold=0 piano-wifi=6 piano-wifi-iova=1 piano-wifi-band=0`, RAM boots with the usual reset backstop (300 s):

| # | Boot args (beyond the base) | Gate | Then |
|---|---|---|---|
| 5a | none: the fifth round's defaults (`rddm=1 defer=1 wake=1`) | level 4 as in 4a plus the `crash dump: 9 segments … given to the chip before the boot` and `channels 20 and 21 planned before the boot` lines; after the ladder, the held lines: `mission mode` at about +480 ms, `device wake asserted`, `both channels run … N ms after level 5 began` with N in the tens of milliseconds, `router: the chip said hello …`, the service line, steps 502–518 with `3 indications`, then level 6's gates (§8.6) | passes level 5: the host's 2 s of silence was the cause (inference made evidence by 5b, below); stops with `crash:` lines: §10.24's table — the failure reason line is the answer, whatever the step was |
| 5b | `piano-wifi-defer=0 piano-wifi-wake=0` | the control: 4a's host behaviour (every line printed at once, no wake doorbell) with the crash dump armed and the new MHI layout. If the console stalls again after mission mode and the firmware dies, the `crash:` lines give the reason for 4a's death | read with 5a: 5a passing and 5b dying names the silence (or the wake doorbell) as the cause; both dying with the same reason names something neither changes (the reason line says what); both passing means either that 5b's console did not stall this time (the USB console's per-line timestamps show the gap after mission mode, or its absence) or that the MHI layout or the channels' early fields were it; `ctxearly`/`socreset`/`msi` remain the A/B for P18 |

**After §10.25 (the sixth round).** One boot, the desktop image (`desk-dev`) with `DESK_EXTRA="piano-wifi=6 piano-wifi-iova=1"`:

| # | Boot args (beyond the image's) | Gate | Then |
|---|---|---|---|
| 6a | `piano-wifi=6 piano-wifi-iova=1` (the defaults: `deadline=60000`, `defer=1`) | the held lines coming out while the ladder runs (batches with `[+N ms]`, not only at the end); level 4 as in desk-dev14; then `63 receive buffers posted (channel 21's ring written to element 63, 1 doorbell(s))`, `router: the chip said hello first and was answered`, `our answer to the chip's hello completed on channel 20`, `the router is up … service 69 instance 0x101`, steps 502–518 with `3 indications`, the ring line with `0 event(s) dropped`, then level 6's gates (§8.6) | stops at 502 again: the ring line says which side did not move (channel 21's read pointer, the data event ring's positions, the hello's completion); a refusal or timeout in 502–514: §8.5's step meanings; `crash:` lines: §10.24's table (the host capability's sleep-clock bit is the first suspect if the firmware dies right after step 504) |

6a ran as `ram-v32-probe` and on the installed v3.2a (the seventh round): level 5 reached step 509 and stopped there (P21, §10.26).

**After §10.26 (the seventh round).** The installed system (its ESP carrying `piano-wifi=6 piano-wifi-iova=1`), or any RAM image
with this driver and those two arguments. The firmware files are read at run time from the tablet's own `modem_a` (`image/peach/`,
else `image/`): `amss20.bin`, `regdb_xiaomi.bin`, `bd_p81.elf`, `phy_ucode20.elf`, `aux_ucode20.elf`, `qdss_trace_config_v2.cfg`
(the last may be absent); nothing for Wi-Fi is staged on the ESP or in a RAM image's addon.

| # | Boot args (beyond the image's) | Gate | Then |
|---|---|---|---|
| 7a | `piano-wifi=6 piano-wifi-iova=1` | level 4 and steps 502–508 as in the seventh round; step 509's four `answer to 0x0025 … regdb_xiaomi.bin segment K of 4 … (accepted)` lines, the last with `the last`; step 510's board line (`bd_p81.elf for board 0xff`, 125272 bytes, 21 segments, kind 1) and its 21 accepted answers; 511 and 512 accepted; 513 accepted (or `refused` / `not answered`, which no longer ends the level); `indication 0x0038: the firmware is ready`; the `flow control:` line with as many RESUME_TX sent as asked for and `none owed`; with `piano-wifi=6` (`mode=1`) steps 515–517 accepted, `step 518: done`, then level 6's gates (§8.6: the transport's ready, the service-ready event, the scan's list) | a `(refused)` answer: its result/error and the segment name the message the firmware rejected (the board file's segments: the file or the kind; 511/512: the microcode's address or size); `unanswered` with `flow control: … one still owed` or fewer sent than asked: §10.26 is not finished; `unanswered` with the flow-control line healthy: the firmware itself stalled, and the `dma:` lines after it say whether its DMA left the window (an SMMU fault in bank N with a FAR) |

7a ran on the installed v3.3d (23:33, 2026-11-12): level 5 finished (the firmware ready at +675 ms) and level 6 reached its
virtual device and its scan, where the firmware asserted (P22, §10.27).

**After §10.27 (P22).** The same image or ESP, with the radio's set-up on. Boot 8a needs **no new argument at all** (the
default `piano-wifi-l6pre=63` is Ubuntu's whole order); 8b adds one, so the kernel's command line grows by at most 21 bytes.
`piano-wifi-l6regwait`, `piano-wifi-l6pace` and `piano-wifi-l6hold` have working defaults and need not be given.

| # | Boot args (beyond the image's) | Gate | Then |
|---|---|---|---|
| 8a | `piano-wifi=6 piano-wifi-iova=1` (the new default `piano-wifi-l6pre=63` needs no argument) | level 4, level 5 and steps 601–606 as in v3.3d; then the four new lines in this order — `step 620: the radio's hardware mode`, `step 621: the radio's parameters`, `step 622: waiting for the regulatory event` (or straight to 623 when the event is already in), `step 623: the channel list`, then `step 607`, `608`, `609`; `level 6: the regulatory domain: country "…" … N rule(s)` with its rules; `level 6: the channel list: K channel(s) (P passive, R radar)`; `level 6: the radio's set-up: hardware mode sent (accepted), 6 of 6 parameter(s), the channel list sent from the firmware's regulatory event; the virtual device's address comes from …`; and then §8.6's gates: `started 1`, the scan's events, **at least one network** | the firmware asserts again: the `crash:` lines give the reason file and `level 6: … the last command went out at step N` names the command before it — 623 the channel list, 607 the device, 608 the scan. Next boot then: `piano-wifi-l6pre=127` (`0x7f`: the same without the scan): if it lives through the hold, the scan is the assert and the channel list or the data path is the gap; if it dies at 607 with a usable address, the device itself needs the data path (HTT, §10.20/§10.27) |
| 8b | `piano-wifi=6 piano-wifi-iova=1 piano-wifi-l6pre=127` (`0x7f`) | as 8a up to `step 607`, then `step 624: holding after the virtual device`, `the teardown: … the virtual device deleted`, `step 611: done` and no crash line | a crash during the hold: the device alone asserts the firmware (the address and the mode are then not the cause and the data path is next); a clean hold: the scan is where the firmware dies, and 8a's channel list did not satisfy it |

Both answer their questions on the installed boot as well as in a RAM image; neither needs the scan to find a network to be
useful, because the gate is the firmware living past step 607.

8a ran at 02:05 on 2026-11-13 (the SPMI-lock probe): no assert, the scan complete, **0 management frames** (P23, §10.28).

**After §10.28 (P23).** The same arguments: the new default `piano-wifi-l6pre=447` (`0x1bf`) adds the receive buffers and the
diagnostics, so **no new argument** is needed.

| # | Boot args (beyond the image's) | Gate | Then |
|---|---|---|---|
| 9a | `piano-wifi=6 piano-wifi-iova=1` (built for the 27.3 line as `ram-k273-wifi-p23`: the installed 27.3 entry's line with `data-persist=0`, `piano-reset-after=900 piano-reset-to=bootloader`, the store seed, the 0066 kernel, the driver arguments in `/chosen/piano-boot-args-ram`) | level 6 as at 02:05, with in addition: `step 625: connecting the firmware's data interface` before 602; after the first scan `step 626: the radio's statistics`, `step 627: the data interface's version`, `step 628: the receive rings`, then a second `608`/`609` and `626` again; then `level 6: the data interface: connected, version 3.N … rings announced 6 of 6; refill: 511 buffer(s) given, the firmware's tail at T …`; `level 6: scan round 1 (before the receive buffers): … 0 management frame(s)`; `level 6: scan round 2 (with the receive buffers): … M management frame(s), K network(s)`; the `round N visits:` lines and the `round N radio:` lines; `events by identifier:`. **Gate: K ≥ 1 in round 2** — and on the tablet `piano-wifi` lists the owner's networks (names are no longer in the log) | **Round 2 also 0** and the visits show busy/receiving cycles: if the refill tail `T` moved and `MPDUs to the firmware` is > 0, frames reach the firmware and something after it drops them; if `T` stayed 0, the firmware never took the buffers (the ring's set-up or its kind is wrong — bit 9, `piano-wifi-l6pre=959`, is the A/B of the return manager). **No receiving cycles at all** in either round: the radio does not hear (RF, the board file, calibration). **An assert** after step 628: the `crash:` reason and the last command name it; boot 8b's `l6pre=127` (no rings, no scan) is the control |

**After §13.3 (level 7, the join).** A 27.3 RAM image built like boot 9a's (`ram-k273-wifi-p23`), with `piano-wifi=7` instead of 6.
The join profile is written over the root shell after the boot (the image never carries it): see the steps in the table's first
row.

| # | Boot args (beyond the image's) | Gate | Then |
|---|---|---|---|
| 10a | `piano-wifi=7 piano-wifi-iova=1` (all other level 6/7 switches at their defaults: `l6pre=447`, `l7=63`, `l7wait=600000`, `l7hold=30000`); after the desktop, as root: place `/private/var/db/piano-wifi/profile` (700/600, root:wheel; the owner's 5 GHz network, `security=wpa2-psk`) and run `/usr/local/libexec/piano-wifi-join` once | level 6 as in 9a up to round 2 (`K ≥ 1` networks), then `step 629: the scan is done: the virtual device stays for level 7`, `level 6: the resource PianoWifiLink is published` and PianoWifiEthernet's `the interface takes the virtual device's address (its first three bytes 02:50:49)`; `stage 700`, the two `level 7:` option lines, `step 701: waiting for the join profile`; after the helper: `join profile set (generation 1): a network name of N byte(s), WPA2-PSK with a passphrase ...` and `level 7: the join profile was handed over after N ms`; then `step 702`, `level 7: the network: entry E of the scan list (C access point(s) ...): F MHz, channel X (5 GHz), signal S dBm` and `its elements: ... RSN (group 4, pairwise 0x10, key management 0x4 ...)`; `step 704`, `705`, `706` with `the data interface named the peer: peer P`, `707`, `708` with `the device's start response: status 0 (started)`, `709` with `frame 1: acknowledged` and `the authentication answer: status 0 (success)`, `710` with `frame 2: acknowledged` and **`the association answer: status 0 (success), AID A`**, `711` with `the peer's association confirmed`, `712`, `714`; then (WPA2 without piece 3) `the association ended: a deauthentication (or disassociation), reason R (...)` a few seconds later, `715` with the teardown's four pieces, `step 716: done`, the two summary lines and `level 7: the data interface: ...`. On the tablet: `ifconfig en1` shows `ether 02:50:49:41:4e:01`; `ioreg -l | grep piano-wifi-level7` the end state. **Gate: the association answer with status 0 and an AID** | **No profile handed over** (`step 701` until `no join profile was set` after 10 min): the helper is missing from the image's Data or refused the file — its log `/private/var/log/piano-wifi-join.log` says which. **`not in the scan list`** after two scans of its own: the name in the profile differs from the network's bytes, or the network is hidden. **`security ... not supported`**: the network requires management frame protection or is WPA3-only (the `its elements` line's numbers say which). **No start response / `refused to start`**: the start command's channel or mode; next boot `piano-wifi-l7=61` (`0x3d`, without the peer data set-up) to see whether 707 is the cause. **Authentication not answered** with `frame 1: not acknowledged`: the frame never reached the air (the device's channel, the transmit path); acknowledged but no answer: the answer did not come back up (the receive buffers: `refill: the firmware's tail` and the radio rings' fill in the last line). **Association refused**: the status names why (18: our rates; 10 or 40: an element of ours). **A crash after 710** (`crash:` lines, `the last command went out at step N`): the first data frame (EAPOL) met the unprogrammed reorder rings — level 8's rings are the fix, and `piano-wifi-l7=61` the control |


**10a ran on 2026-11-13 15:20-15:49 (`ram-k273-wifi-10a`, `7090599c...`; private report `host-checks/20261113-tablet-10a`).**
Level 6 and the hand-off as expected, the profile handed over through the sysctl both times, no crash. Attempt A (an 11-byte
name): not in the list after level 7's two passive scans; the list had nameless entries (`it does not say its name`) of the
same access points, so the network most likely hides its name, which a passive scan cannot learn. Attempt B (an 8-byte name, 4
access points): steps 704-708 passed (peer 2, hardware peer 58, 17 reorder queues, start status 0), then all three
authentication frames came back with completion status 3 (not acknowledged). **Cause (from the log, proven by the code):** the
chosen entry said `channel 40 (5260 MHz)`: its beacon names channel 40 (5200 MHz) but the radio heard it while tuned to 5260
MHz (a -39 dBm neighbour leaking into the next channels), and the scan entry kept the tuned frequency; the device was started
and the authentication sent on 5260 MHz, where the access point is not. Fixed: a network's frequency is its own channel's (the
DS parameter set, else the HT operation, else the header's channel number; §13.3). Also: the Wi-Fi interface is **en0** on the
27.3 line (en1 is the USB network), and PianoWifiEthernet never found PianoWifi's properties (unregistered service; fixed).

**After 10a.** The same recipe (`out/ram-k273-wifi-10b`, built 2026-11-14 08:43 from branch commit `2afe838`: 237281280 bytes,
sha256 `18f142f5421edc49614e7c123ddc77435a8e1025339394e15ed5e12b3019f61d`, `verify-ram-images.py` PASS; the 27.3 collection
`550c006f...` with the 0066 kernel, 26 native kexts; `data-persist=0` on the store seed `89860516...`; `piano-reset-after=900
piano-reset-to=bootloader`): `piano-wifi=7 piano-wifi-iova=1`, every level 7 switch at its new
default (`l7=0x73f`, `l7cands=4`, `l7scans=3`, `l7file=1`). The profile is placed as before (root:wheel, `600` in a `700`
directory); the kext reads it itself within 2 s (no helper needed; the sysctl still works).

| # | Boot args (beyond the image's) | Gate | Then |
|---|---|---|---|
| 10b | `piano-wifi=7 piano-wifi-iova=1` | as 10a up to `step 701`; then either `join profile read from its root-only file (generation 1): a network name of N byte(s), WPA2-PSK with a passphrase ...` or the sysctl's `join profile set ...`, and `level 7: the join profile was handed over after N ms`. **Hidden name (attempt A's profile):** `level 7: our scan 1: active, a probe naming the profile's network, 5 GHz (not radar) on 4+ channel(s)` (and scan 2 on 2.4 GHz), then `step 702` with `level 7: the network: entry E ... (C access point(s) carry the profile's name ...)`. **Each candidate:** `level 7: candidate K of N: 5 GHz, not radar \| 5 GHz, radar (passive) \| 2.4 GHz, F MHz channel X, ...`, its steps 705-708, `level 7: frame D check whole (0xfff): ...`, `level 7: frame D: acknowledged`, `the authentication answer: status 0`, then 710: **`level 7: the association answer: status 0 (success), AID A`**; a candidate that fails: `level 7: candidate K ended: <reason> (<class>); the next one follows its teardown`. Level 6's list lines show `(5200 MHz, heard at 5260 MHz)` where a network was heard off its channel, and level 7's `its frames were heard while the radio was on ... MHz` for the chosen one. `PianoWifiEthernet: the scan list has N network(s)` and `the interface takes the virtual device's address (... 02:50:49)`; on the tablet `ifconfig en0` shows `ether 02:50:49:41:4e:01`. **Gate: the association answer with status 0 and an AID** | **Every candidate's frames `not acknowledged`** with the check `whole`: the frames are right and do not reach the access point — compare the classes: only radar/passive ones failing points at transmission on a passive channel; all classes failing at the transmit path (power, the device's channel) — next boot `piano-wifi-l7=0x63f` (no power limits) as its A/B. **A check `NOT whole`**: the bit named `NO` is the bug. **Hidden name still not found** after both directed scans: the probes were not answered (the network may not answer a directed probe on that band); `bssid=` in the profile pins the access point. **A crash after 710**: as 10a's last column |

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
   GPIO write. (level 1 reads the pin, level 2 reports it, nothing drives it) Measured since: the pin reads `mode 00 … pull 04`
   (a digital input with a pull-down: no 32 kHz out) in every boot that read it, and the chip reached mission mode without it, so
   its boot loaders need neither; whether the running firmware does before the host capability tells it to use its internal
   sleep clock is §10.24's leading inference for P19.
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
   PianoInput's/PianoPower's/PianoDwc3's). The PMU is **shared with Bluetooth** through
   `package/include/PianoWcnRails.h` (`drivers/bluetooth.md` §7): its two words are PianoA830's, the PMU lock orders the two
   radios' power changes, level 1's PMIC survey and level 2's rails run under it, and every path where this driver gives the
   chip up (a level 2 to 6 failure, or the suspend hook) gives its owner bit back — with the enables withdrawn only at
   `piano-wifi-release=1` and only when no owner is left. Every vote of ours still only ever adds.
8. Licensing of Ath12kWifi (GPL code compiled into an XNU kext, no LICENSE in its repository): an owner decision before levels 7 and 8
   use it; it would stay an external build input, never in this repository (§11).
9. Heat and power: Wi-Fi at full power with no thermal driver; sessions stay attended.
10. Level 4's DMA window is 24 MiB of XNU memory below 2^36 (64 MiB at level 5), mapped write-combined, and it is **never
   freed**: once the chip has been given those addresses, nothing but a reset guarantees it has stopped writing to them. A failed
   level 4 or 5 therefore ends with WLAN_EN low, PERST# asserted and the window still held for the rest of the session.
11. **32-bit addresses.** This chip's own driver allows it 36 bits for the firmware's regions but only 32 for the rings of the data
   path, and piano's XNU memory is all above 4 GiB, so a stream that bypasses the SMMU (levels 3 and 4) can never produce an
   address level 6 will need. `piano-wifi-iova=1` answers it: stream 0x1401 gets a translating stage-1 context bank whose 32-bit
   window (`0x20000000`, 2 MiB blocks, attribute Normal non-cacheable to match the write-combined mapping) maps the window, and
   every address handed to the chip is the window's. The bank is found and programmed the way PianoDwc3's and PianoUsbHost's are
   (a free group, or the bypass entry level 3 left for the same stream redirected in place; a free bank from one below the top),
   its two-page table is a buffer of its own so that the chip cannot reach its own translation tables, and every block is checked
   to translate back to the window before the chip is told anything. What is not settled from here: whether this firmware insists
   on 32-bit addresses for the regions it asks for at level 5 as well (boots 4 and 5 of §8.8 answer exactly that), and whether the
   hypervisor lets EL1 program that bank at all — level 3's bypass bank is the precedent, and a refusal shows as a read-back
   failure, not a hang. (needs-boot)
12. **What level 5 starts.** Without `piano-wifi-mode=1` the level stops at the ready indication and the firmware's radio is not
   started; with it, the last three messages describe the host's copy engine and set the mode, and the firmware then expects the
   host transport's control messages on those pipes — which level 6 provides and level 5 does not. The chip is left in that state
   until the session ends (boot 6 of §8.8 is attended for exactly that reason), and nothing in level 5 writes a ring the firmware
   could follow into host memory other than the regions it asked for.
13. The crash dump (RDDM) **is read since the fifth round** (§10.24, `package/PianoWifi/WifiRddm.h`): the table is given before the
   boot, the firmware writes its dump into nine 512 KiB segments of the window when it dies, and the log gets a decode of a few
   lines (the header's files, the failure reason). The dump itself never leaves the tablet's memory: nothing writes it to a file,
   and nothing of it beyond the decode's short lines is printed or committed. Open: whether this firmware's dump carries the
   failure-reason file the vendor host looks for (the decode falls back to at most four error-like strings of the first segment
   when it does not), and whether its header's first two words are in the order the vendor host has (the decode takes either).
14. **The suspend hook is one way.** The power contract (`package/include/PianoPowerContract.h`) is answered from level 2 on, and
   the only state change this driver makes is `kOff`: the scan is stopped, the virtual device deleted, the rings disabled,
   WLAN_EN taken low and PERST# asserted — and **nothing is brought back on the wake**. The rails stay voted (Bluetooth shares
   them, §10.7) and the Wi-Fi path is off until the next boot; `en1` stays published and down. That is deliberate at a
   bring-up level: it is the safe thing for the battery (a radio the owner cannot see is not left running), and a resume path
   would have to redo levels 2 to 6 from a thread the suspend woke. The hook also asks the ladder's own thread to stop and
   waits up to two seconds for it, rather than touching the hardware under it; a thread that does not finish in that time
   leaves the rings alone and only the pins are taken down. Stopping it takes longer than the one second a synchronous power
   answer may take, so `setPowerState` answers with a four-second deadline and acknowledges when it is done, on a thread of its
   own (`docs/power.md` §4). (needs-boot: boot 10 of §8.8)
15. **Two words of the resource configuration are left zero on purpose.** The peer-metadata version (bits 5:4 of the second
   flag word) and the "extended regulatory event" bit (bit 4 of the host service flags) are what this chip's own host sets from
   its service bitmap. Level 6 reads no service bits and sets neither: the first decides how a peer identifier is packed in the
   data path's descriptors (level 8), and the second only chooses which of two regulatory events the firmware sends (level 7
   reads either). Both must be set from the bitmap before the data path is built.
16. **The firmware's memory requests at level 6.** Its service-ready event may ask the host for memory chunks, which the
   initialisation command would have to carry. This chip's own driver never allocates any (its count is always zero, which is
   why the array this driver sends is empty), so a firmware build that does ask ends level 6 at step 604 with "the firmware
   asked for memory this level does not hand out" rather than sending it an address it never planned for. If a boot shows a
   non-zero count, the regions go in the window the way level 5's do, and the command's array grows by one structure each.
   (needs-boot)
17. **The regulatory domain and the channel list.** Level 6 sends the channel list inside the scan request and never sends
   `WMI_SCAN_CHAN_LIST`, which is what a driver normally does after the firmware's regulatory event. The firmware is free to
   skip a channel its domain does not allow, so a passive scan may come back with the 2.4 GHz band only (boot 8 of §8.8 is
   exactly that measurement). Level 7 reads the regulatory event and sends the channel list before it starts anything.
18. **The chip's registers through one window.** The copy engine's rings live above BAR0's direct range, so every pointer write
   goes through the window register at BAR0 + `0x3278`: two accesses, which are only safe because this driver has exactly one
   thread and no interrupt. The shadow registers of §12 would make each pointer write a single access to a BAR0 offset, and
   `piano-wifi-shadow=1` already sends the list — but the list it sends describes the rings of level 8's data path, not level
   6's four pipes, so level 6 writes the real registers. A level that adds the data path should send a list that matches what it
   actually creates and then use the shadows.
19. **The shadow registers are the fallback for step 601.** This chip's own driver, when the firmware has been given the
   shadow list, writes a ring's pointer to BAR0 + `0x8fc` + 4·index instead of the ring's own register — including for the copy
   engine's rings. Level 6 writes the real registers through the window instead (§10.18), which is the documented path for
   every other register of this chip. If the first boot's step 601 times out with the ring lines looking right, the next thing
   to try is a shadow list that describes **level 6's** four pipes (level 5's `piano-wifi-shadow` list describes level 8's data
   path, not these rings) and pointer writes through it. (needs-boot)
20. **No data path at level 6, and the firmware knows it.** The firmware's initialisation tells it how many peers and traffic
   identifiers the host has room for, but no receive ring of the data path exists: a frame that is not a management frame has
   nowhere to go. The scan needs none (its results are management frames on the message interface), and nothing was observed to
   stall at the host test's level — but on hardware the firmware may log about the missing data interface (HTT), and a level 6
   boot should not be left running for long (§8.7).
21. **P16: the boot loader never took the image, and the PCIe NoC (2026-11-12).** Four boots (wifi-l4b and l5b with physical
   36-bit addresses, wifi-l5ic and l6c with the 32-bit window translating through bank 81) ended alike: MHI open (MHIREGLEN 0x100,
   BHIOFF 0x100, BHIEOFF 0x224), EE PBL, TCSR 40210200, the doorbell rung, then 20 s, 3550 polls, no event, `EE PBL, state 0, BHI
   status ffffffff, error code ef0e0003 (a phase timed out)`. State 0 is MHI's RESET: never READY, and no SYSERR in any poll (a
   SYSERR ends the phase with its own message); EE read PBL at the start and at the end, so the device answered and BHI sits
   where BHIOFF says.
   Checked against the MHI host's behaviour (Ubuntu's `drivers/bus/mhi`, behaviour only), with the evidence:
   - **Offsets** (code): BHI + 0x08 IMGADDR_LOW, 0x0c HIGH, 0x10 IMGSIZE, 0x18 IMGTXDB, 0x20 INTVEC, 0x28 EXECENV, 0x2c STATUS,
     0x30 ERRCODE, 0x34–0x3c ERRDBG1–3, 0x40 the serial number; BHIe + 0x2c/0x30/0x34/0x3c/0x44. The "BHI status" read was BAR0 +
     0x12c and the error code BAR0 + 0x130, the EE that read 0 BAR0 + 0x128. All as the MHI host has them.
   - **Order** (code): the host writes BHI's status 0, the address high, then low, the size, then the doorbell with a non-zero
     30-bit session number; INTVEC 0 at power-up and again after a reset. This driver had everything but the status clear.
   - **The address** (log, code): with `piano-wifi-iova=1` the arena's bus base is the window, the image at `0x20000000`, the bank
     translating to the physical window; without it the physical `0x9aa400000`. Both failed alike: the address is not it.
   - **Size, alignment, contiguity** (log, code): 524288 bytes, the host's `sbl_size` (the first 512 KiB of the file), in one
     physically contiguous window, 512 KiB aligned (the arena's rule); the second ELF in 13 segments of 512 KiB, the vector table
     13 × 16 bytes, as the host builds them.
   - **Caches** (code): the window's cacheable alias is cleaned and invalidated to the point of coherency after zeroing, the image
     is written only through the write-combined mapping, a DSB precedes the doorbell, and the window bank's attribute is Normal
     non-cacheable, so the chip reads DRAM. A stale image would anyway fail the boot loader's check with an error, not silence
     (inference).
   - **MSI / INTVEC** (inference): the host enables MSI and lets vector 0 wake its wait, but BHI's progress is a register it can
     poll, and nothing in its path makes the device wait for an interrupt being enabled. This driver polls and leaves MSI off.
   - **`ef0e0003`**: no public decoding (the MHI specification leaves the error code to the implementation). The same value with
     the same all-ones status in four boots and both address modes, and a status nothing had ever cleared: most likely both are
     the registers' power-on contents and the boot loader never wrote either (inference).
   What Ubuntu does before BHI that the native path did not (read for behaviour: `pcie-qcom.c`, `interconnect/qcom/sm8750.c`,
   `bcm-voter.c`, ath12k `pci.c`): **(a)** pcie-qcom votes the "pcie-mem" path before the controller's clocks (its authors note
   that some Qualcomm platforms need the bandwidth set first) and at width × speed once the link is up; on SM8750 that path is
   MASTER_PCIE_0 → qns_pcie_mem_noc, whose bus clock manager **SN4** is not keep-alive and is that NoC's only one, then the gem
   NoC (SH0) and DDR (MC0), both keep-alive. Nothing votes SN4 natively; sheng (SM8550, hardware-verified to traffic) votes its
   counterpart (SN7) at link-up "since nothing has done that here". Configuration, BAR and the chip's register window go the
   other way (CPU to chip, keep-alive managers), which is everything levels 3 and 4 showed working; the chip's read of the
   image is the first thing that crosses the PCIe NoC. A read that never completes leaves the boot loader waiting and BHI
   untouched, address mode regardless. **Likely cause, inferred**; level 3 now votes SN4 (`piano-wifi-noc=1`, the vote at Gen ×
   2 lanes, the aux data from cmd-db, average 0 as pcie-qcom asks) and withdraws it wherever the chip is given up and in the
   suspend hook (PCIe0 is Wi-Fi's alone; it costs power only while the chip is up). **(b)** The status clear before the
   doorbell (fixed; an error now needs bits 31:30 = 3 with MHISTATUS answering and the clear taken or the value changed;
   ERRDBG1–3 read with the code). **(c)** ath12k's chip-side reset at every power-up (the PCIe interrupt clear, for an
   interrupt a hot reset leaves pending, the warm-boot entry, the reset cause, the SoC global reset, MHICTRL's reset):
   `piano-wifi-socreset=1`, off by default, boot 3b of §8.8. **(d)** ASPM is disabled during the download (never enabled
   natively) and MSI enabled (above). Not done natively and not found necessary: the receive vector's clear (only the crash
   dump uses it). The diagnostics added so that the next failure says where the read went: §8.4. The A/B of (a) is
   `piano-wifi-noc=0`. (needs-boot: §8.8 boots 3a, 3b)
   **Corrected by the fourth round (§10.23):** (a) was not the cause (3a, 3b failed with SN4 taken), and the reading of `ffffffff`
   / `ef0e0003` as untouched power-on contents was wrong: 3a and 3b read BHI's status 0 before the clear and saw the boot loader
   write both 45 ms after the doorbell. The morning's boots, whose last values were the same, most likely saw that refusal too and
   waited 20 s because the old rule ignored an all-ones status. The receive vector clear listed above as "not found necessary" is
   done since the fourth round, as part of the MHI host's pre-boot path.
22. **P17: the GPU's bring-up and the DRV-2 lock (wifi-l5ib, 2026-11-12).** PianoA830's `S1 step 0x10 reason 9 ... reg 0x1 value
   0xfffffffffffffff0 expect 0xffffffff` decodes (Adreno830Start.h, Sm8750Rpmh.h) as: S1's mx/cx vote, reason RPMh, the RPMh
   result's step 1 (the lock), status -16 (busy), no TCS chosen: A830 waited its whole 2 s for `sm8750_rpmh_drv2_owner` and never
   reached a TCS (evidence). The console's own order (pstore) is: our smpd4 line, A830's S1 begin, A830's failure 2.96 s later,
   then our smpi7 line "completed after 1 ms", with no "the DRV-2 lock is held" line of ours (the lock was free when we took it)
   and nothing between two of our votes but the loop (evidence). So **our smpi7 transaction held the lock across A830's whole
   wait**: it polled with `IOSleep(1)` (the "1 ms" counted polls, not time) and printed its line with IOLog before releasing — a
   sleep and a console write under a word another driver spins on for at most 2 s. Which of the two took seconds is not known
   (inferred: boot-time scheduling of the sleeping thread, or the console). Fixed: the poll is a busy wait (10 µs steps against a
   100 ms clock), the line comes after the release with the hold time in it, and the reclaim clears WAIT_FOR_CMPL too — A830 takes
   a TCS as idle only with CONTROL, CMD_ENABLE and WAIT_FOR_CMPL all 0, so a TCS 2 that Wi-Fi or Bluetooth had used stayed "busy"
   for A830 for the rest of the session (harmless while A830 finds TCS 0 or 1, a TCS lost otherwise). PianoBluetooth's
   `BtRpmh.h` (also PianoDwc3's cold path) slept under the lock the same way: it now takes a `delayUs` callback and both owners
   pass IODelay (host test: no sleep, 10 µs steps, the reclaim). Which TCS each uses: A830 0, 1, 2 in that order; Wi-Fi, Bluetooth
   and PianoDwc3 2, 1, 0; the lock serialises them, so no two program a TCS at once, and a TCS that timed out stays triggered and
   is never idle again for anyone. Not changed (other packages, noted for their owners): PianoPower's `PianoAdsp.cpp` voteLocked
   logs every write under the lock, and PianoKeyboard's voteLocked sleeps 1 ms per poll under it. (needs-boot: any boot with
   `piano-wifi` ≥ 2 and the GPU: the `rsc:` lines' hold times)
   wifi-3a and 3b (2026-11-12): fourteen `rsc:` lines each, the lock held 32–94 µs, taken after 0 waits; PianoA830 ran to stage 15
   in both. Its S1 had finished before level 2 began in both logs, so the overlap that failed is not re-tested yet (evidence for
   the hold times, not for the race).
23. **P18: the boot loader refuses the boot loader image itself (wifi-3a, wifi-3b, 2026-11-12).**
   Evidence, from the two logs (USB console and pstore, the serial number masked in the dumps):
   - BHI's status read `00000000` before this host's clear (the clear took) in both. After the doorbell (session `0xe63d` in 3a,
     `0xf2ad` in 3b) nothing changed until the poll at **+45 ms**, which read BHI status `ffffffff` (bits 31:30 = 3, an error)
     and error code `ef0e0003`. ERRDBG1–3 read `20000000 00000000 00080000`: IMGADDR_LOW, IMGADDR_HIGH and IMGSIZE as written.
     IMGTXDB kept the session number, INTVEC 0, EE `PBL` and MHISTATUS `00000000` (RESET, no SYSERR) to the end.
   - The `dma:` lines, read 3 s after the refusal in 3a and 2 ms after it in 3b: endpoint command/status `00100006` (status
     `0010`: the capability list only), device control/status `00102810` (status `0010`: AUX power only, none of the four
     error-detected bits), AER uncorrectable and correctable 0 at both ends, root port secondary status 0, PARF interrupt status
     `00002000` (link up only), SMMU sCR0 `00290406`/`002d0406` with sGFSR 0, bank 81's sctlr `00000027` and fsr `00000400` (the
     format field only: no fault; its FAR the bank's power-on contents).
   - 3b ran the old partial chip reset first: global reset `00000004 -> 00000004`, warm entry 0, reset cause 0, MHISTATUS, EE and
     BHI status 0 afterwards; the result was identical to 3a's to the register.
   - The morning's boots (wifi-l4b/l5b with physical addresses, l5ic/l6c with the window) ended with the same status and code;
     the old rule ignored an all-ones status and waited 20 s, and nothing read the status before the doorbell.
   Reading (inference unless marked):
   - **No PCI-level error came with the refusal.** A read the root complex or the SMMU refused comes back as an Unsupported
     Request or Completer Abort completion, which sets the requester's "received master/target abort" status bit and a device
     status error bit; a read that is never answered ends in a completion timeout (AER bit 14), also with a device status bit; a
     poisoned completion sets AER bit 12. None is set at either end; the AER status registers record an error whether or not
     it is masked or reported (PCIe base specification), and 3a read them 3 s after the refusal, long after any completion
     timeout (50 ms at most by default) would have expired. So either the boot loader never issued a read, or every read it
     issued was answered with data. sheng saw a refusal *before* any transfer (a page-aligned address) leave the debug words at 0; here they name the
     transfer, so most likely the transfer had started.
   - **The bytes, by structure (evidence, offline):** the stock file Ubuntu pins (`xiaomipad-8pro-mainline` piano-wlan.json,
     6930496 bytes, the size modem_a's has) is a signed 32-bit ARM ELF (seven program headers, the hash segment among them,
     0x2e630 bytes of segments) padded with zeros to 512 KiB, then a Hexagon ELF exactly from 512 KiB to the end. BHI's 512 KiB and
     BHIe's "second ELF" are what the mainline MHI host does with such a file (`mhi_fw_load_handler`: `sbl_size` over BHI; when an
     ELF header follows at `sbl_size`, only the rest over BHIe), and ath12k configures this chip with a 512 KiB boot loader and
     512 KiB segments. The size register is the 512 KiB the host writes too (it caps `sbl_size` at the file's size), the address
     registers are written high then low as the host does, the session number is a non-zero 30-bit value as the host's is, and
     the address is 32-bit as the host's coherent mask makes it. Whether the window **holds** those bytes was never measured:
     NativeUfs's zero-copy read DMAs into the write-combined window's pages and invalidates by the destination address, and
     nothing writes the window through a cacheable alias after its clean, so it should (inference); the fourth round hashes it.
   - **What Ubuntu does before the doorbell that this driver did not** (ath12k `pci.c` power-up, the MHI host's
     `mhi_prepare_for_power_up` / `mhi_async_power_up` / `mhi_ready_state_transition`, behaviour only):
     (1) ath12k's whole power-on `sw_reset`: besides the four steps 3b ran, the chip's PARF LTSSM word set to `0x111`, the
     hot-reset bit of its GCC (`0x1e65304`, the QCC2072 register set WCN7860 uses), the BHIe vector doorbell and status words, and
     the **QFPROM power control's VDD4BLOW bit cleared** — the fuse-blow supply; the boot loader checks the image's signature
     against key hashes it reads from those fuses, so this is the step that bears most directly on a signature refusal
     (inference); (2) the BHIe receive block zeroed (its power-on values are undefined; piano's read garbage there);
     (3) MSI enabled before MHI starts; (4) the MHI context registers, including the host's control and data address ranges, written
     only once the device is READY — this driver wrote them before the doorbell (from the donors' note that the loader wanted them),
     with a 36-bit range: the range registers are the one thing in this list the boot loader's DMA could consult (inference);
     (5) the root port's unused I/O and prefetchable windows closed by the enumeration (bridge housekeeping: a window over the
     image's address would have refused the reads with an error, which did not happen, so this one is not the cause).
   - Not different, checked: the offsets (BHIEOFF `0x224` also matches ath12k's absolute vector offsets `0x360`…`0x39c`), the
     order of the BHI writes, the session number, the size, the 512 KiB alignment, the window's address (sheng's working window
     for the same chip family started at `0x10000000`, so a low IOVA is not it), the file.
   Changed (`WifiPbl.h`, `WifiMhi.h`, `WifiBringup.h`; host tests in `tests/bringup_host.cpp`): (1)–(5) are now the default, each
   with its A/B (`piano-wifi-socreset=0`, `piano-wifi-msi=0`, `piano-wifi-ctxearly=1`; step 307 always). Measured from now on, so
   that the next failure decides between "never read" and "read and refused" (§8.4's table): the RAS DES counters of both ends
   (reads sent and received, completions with and without data, UR/CA/completion timeouts), the MSI sink, the image hashed
   through the window as the chip reads it (the boot loader against `c0e4bb91…`, the file against Ubuntu's `02bbc028…`), and the
   boot loader's fuse-derived identity words (MSM hardware ID, OEM public key hash) as counts and a digest, never as values.
   (needs-boot: §8.8 boots 4a, 4b)
   **Result:** 4a (this path) passed level 4 and 4b (the old one) failed as before after the boot loader had read the image: one of
   (1)–(4) is the cause, which one is not yet separated (§8.8).
24. **P19: the firmware dies about 2 s after mission mode (wifi-4a, 2026-11-12).**
   Evidence, from the USB console log and its per-line timestamps (the pstore copy has bit errors in that stretch):
   - Level 4, the fourth round's path: BHI complete (`8000ec87`) at +46 ms, READY at +58, M0 asked for and the contexts given,
     the second ELF over BHIe complete (`8000ec88`, the session this host wrote) at +426, BHI's error code `cafecace` at +471 (the
     boot loader's own hand-over word, not an error: BHI's status stayed complete), **EE AMSS at +477 ms**, MHISTATUS `00000201`
     (M0, ready) throughout.
   - The very next line from this thread (`mission mode after 477 ms`, a plain IOLog) reached the console **2027 ms** after the
     line before it; the lines between them on the console are the root volume's mount and launchd's start. The summary line
     after it read the environment at **+2503 ms: RDDM**. The TLP counters then: 70389 reads from the chip, all answered with
     data, 19 writes from it, no UR, CA or completion timeout; the MSI sink held the chip's MSI.
   - Level 5's first walk of the control event ring delivered the AMSS environment event and then a **SYS_ERR** state change
     (state `0xff`); no M0 or M1 event came after AMSS. Channel 20's start command got no completion in 2 s; stage 501.
   - Nothing read the dump: no receive vector table had been given (the block was only zeroed), so the reason is not known; the
     SMMU's fault registers are printed only on a level 4 failure and were not read (needs-boot: the crash report now prints them).
   What the MHI host and ath12k do in that window, against what this host did (behaviour only; Ubuntu's kernel, `UPSTREAM`):
   | Item | Linux (mainline MHI host, ath12k, qrtr) | This host in wifi-4a | Now |
   |---|---|---|---|
   | Time from mission mode to the first channel command | the mission-mode transition creates the devices at once; qrtr-mhi's probe prepares channel 20 then 21 and queues the receive buffers: milliseconds | 2 s of nothing (one console line), then a start the dead firmware never answered | all lines held in memory from the doorbell (`defer`); the channels' rings planned before the boot, so only doorbells and two commands are left after mission mode |
   | Device wake (channel 127's doorbell) | asserted at the mission-mode transition (and at every M0 transition and around each command) | never written | asserted at mission mode, held while the levels run, released at the end of a ladder that succeeded (`wake`) |
   | M0 handshake | M0 asked for at READY (after the event ring doorbells); on an M0 event in mission mode every event ring, the command ring with work and every channel rung again | M0 at READY, the same order; M0 events not acted on | the M0 answer in level 5's walk |
   | M1 / M2 | a device that idles (M1) is answered with M2, then woken again if the host still needs it | not handled | M1 answered with M2 and a wake toggle (none was seen in wifi-4a) |
   | M3, time sync | M3 is suspend only; the mainline host and ath12k use no MHI time sync | — | nothing to do |
   | Channel contexts 20 and 21 | the fixed fields (disabled, burst mode disabled, type, event ring 1) written before the power-up; ring and enabled state at the channel's preparation, right before its start command | all zero until level 5, then everything at once | as Linux (`mhiContextsPlan`, `ipcrPrepare`) |
   | Ring alignment | every ring's base a multiple of its length; the context arrays separate allocations | the 4 KiB data event ring (the one channels 20/21 report on) 512 bytes into a page | every ring at a multiple of its length, the arrays on pages of their own |
   | Element sizes, doorbell mode, event rings | 16-byte elements; burst mode disabled on both channels and rings; 32 and 256 event elements, vectors 1 and 2, 1 ms moderation on the second | the same | — |
   | Command ring | 128 elements | 8 (two commands are ever issued) | — |
   | Crash dump (RDDM) | 0x420000 bytes in 512 KiB segments given before the power-up, right after the receive block's clear; on RDDM the host waits for the receive status and collects | block zeroed, no table: the firmware had nowhere to write | as Linux (`rddm`), plus a bounded decode |
   | SYSERR | the interrupt handler reads the state and environment on every MSI; RDDM with a dump table is the crash callback | the state was read only by the next step; a SYS_ERR event was only logged | MHISTATUS and EE read on every level 5 turn and in level 6's loop; a death ends in the crash report |
   | QRTR | the host's name service answers the chip's hello (it never speaks first on a new link), then the chip announces its services | our hello first, then the chip's | the chip's hello awaited 500 ms, then ours |
   | QMI timing | on the WLFW service's arrival: the radio's capability, the indication registration, the host capability, within milliseconds | not reached | the same order (§12), no console in between |
   | Host memory (request memory, M3 dump) | asked for by the firmware after the indication registration; types 1, 3, 4, 9, 0xb answered, others with 0, regions over 2 MiB asked again smaller | not reached | unchanged (§12) |
   | The image over BHIe ("paging") | the full boot chain's segments kept until power-down | complete with our session; the segments stay in the window for the session | — |
   | Board, regulatory, calibration data | QMI downloads after the memory exchange | not reached | unchanged |
   | DMA window | the chip may reach 36 bits; ath12k's rings 32 bits | 64 MiB at `0x20000000`; every read served; faults not read | the crash report prints the SMMU bank's FSR/FAR (a fault there means the chip went outside the window) |
   Reading (inference unless marked): the firmware lived about 2 s with a host that had not yet spoken to it, and died; the
   logged difference from Linux in that window is the host's silence (no wake doorbell, no channel, no hello answered, no host
   capability) — the rest of the table is either identical or not reached. Why silence would kill it is not documented; the most
   specific candidate is the host capability's platform bits, by which ath12k tells a non-Qualcomm platform's chip to use its
   **internal sleep clock**: natively the PMK8550 sleep clock pin reads as a digital input with a pull-down (`mode 00 … pull 04`,
   level 1's SPMI survey, every boot that read it; piano's DT names neither the pin nor a clock for the chip, so Ubuntu most likely
   leaves it the same), so the chip gets no external 32 kHz and depends on that bit; a firmware that
   waits a bounded time for its host and then assumes otherwise would die like this. Only the dump can say (5a/5b, §8.8).
   How to read the crash lines (`crash:` …, after the held lines):
   | What the lines show | Meaning | Next |
   |---|---|---|
   | `the firmware's failure reason (file "…SFR…"): "…"` | the firmware's own account (a source file and line, an assertion, an error name) | act on what it names; it is the answer to P19 |
   | `dump complete`, a header with files but no reason, `error-like text …` lines | the dump arrived but has no failure-reason file; the strings are the best text of its first segment | read them as hints only; the file table (names, sizes) is worth recording in this page |
   | `dump not complete after N ms`, `the dump is all zeros` | the firmware never wrote the dump: the table was not taken (session `not ours`), or it died below its dump code | `crash: the chip is not in its crash dump environment` with it: a system error without RDDM; the BHI error words on the first `crash:` line are then the only account |
   | `dump header not decoded`, with strings | a dump of another shape: the first words are printed | compare the words with the vendor layout (§10.13) |
   | the SMMU bank's `fsr` with a fault bit, `far` outside `0x20000000` + 64 MiB | the chip's DMA left the window | the address names what the firmware wanted: a region it expected from the host |
   Changed (`WifiRddm.h`, `WifiMhi.h`, `WifiChannels.h`, `PianoWifi.cpp`; host tests in `tests/qmi_host.cpp` and
   `tests/bringup_host.cpp`): the crash dump, the held log, the wake doorbell and the M0/M1 answers, the ring alignment, the
   channels' two-step preparation, the health checks, the hello wait (the table's last column), each with its A/B
   (`piano-wifi-rddm=0`, `piano-wifi-defer=0`, `piano-wifi-wake=0`). (needs-boot: §8.8 boots 5a, 5b)
   Result (desk-dev13w/14/15, 2026-11-12 evening, the defaults): no death in the 11 s level 5 ran after mission mode, three boots
   out of three, so the console's hold on this thread was the most likely cause of 4a's (no `defer=0` control among them). The QMI
   exchange (and with it the sleep-clock bit of the host capability) was still not reached: §10.25.
25. **P20: the chip's router had nowhere to answer (desk-dev13w/14/15, 2026-11-12 evening).** Every boot: both channels running
   4 ms into level 5, `0 receive buffers posted`, `no hello from the chip within 500 ms: ours went first`, `hello unanswered, 0
   services announced, 0 packets in` at +10986 ms, stage 502. The firmware alive throughout (no RDDM, no SYS_ERR).
   Cause (proven: the code path, and the host test now reproduces the order): level 5's walk (`pump`) posts receive buffers after
   any turn that saw an event; the turn that saw channel 20's start completion posted all 63 into channel 21's ring while 21 was
   still disabled (its doorbell rung on a disabled channel), and `ipcrPrepare` for 21 then put that ring's write pointer back at
   its base with every buffer still marked busy. The chip started 21 on an empty ring and the "posted after the start" call found
   nothing free. With no receive element the chip's router can send neither its hello nor its answer to ours (MHI moves an inbound
   packet only into an element the host gave it). The model of the host test made the same mistake possible: its turn posted only
   when both channels ran, which the driver never did.
   A second fault behind it, never reached by a boot: a transfer completion event's pointer is the address of the transfer ring
   element it completes (the MHI host keeps a buffer record per element and walks its own read pointer up to the one named), and
   `ipcrPollRing` compared it with the buffers' addresses — every completion in or out would have been dropped, the four outbound
   buffers would have stayed busy after four packets, and no received packet would have reached the router. The model wrote the
   buffer's address into its events, so the test agreed with the code.
   | Item | The MHI host (behaviour, public) | This host before | Now |
   |---|---|---|---|
   | Inbound buffers | queued right after the channel's start completes (all elements but one), one doorbell | posted by any walk, also into the not yet prepared ring; the preparation reset the ring but not the buffers | only on a running channel 21; a start (re)prepares the ring and gives every buffer back (`ipcrStart`); the level stops at 501 if none is posted |
   | Transfer completion | the event names the element; the host walks its read pointer to it, completing each element's buffer; the named one carries the length | matched against buffer addresses | per-ring element records and read pointer (`ChanRing::slot`, `rp`); merged completions give the unnamed buffers back unread (counted) |
   | Codes | EOT/EOB/success complete; overflow is an error with the buffer back; out-of-buffers and doorbell mode complete nothing and the channel is rung again when it has work | only success/EOT considered | as the host; counters for each |
   | Ring-full check | the host's own pointers | the chip's context read pointer | the host's read pointer |
   | Send on a channel that does not run | refused | queued | refused, the claim given back |
   | Event ring doorbells | the high half first | low first (level 4's acknowledgements) | high first (`doorbell64`) |
   | Router header | net/qrtr reads version 1 and version 2 headers and sends version 1 | version 1 only (anything else counted as bad, silently) | both read (`WifiQrtr.c`'s third addition, `UPSTREAM`), version 1 sent; the bad count in the stage 502 line |
   | Diagnostics | — | none | the ring line (both channels and the data event ring, the chip's and this host's pointers), the hello's completion, the services the chip announced |
   Also (desk-dev8n, 2026-11-12 19:41: the boot hung after the doorbell and printed nothing for 30 minutes, because every line was
   held until the ladder's end): the held lines are printed by a helper thread at every phase change and within a second, and each
   of levels 4, 5 and 6 has a hard deadline (`piano-wifi-deadline`, default 60 s) that ends its waits, with the helper's
   `watchdog:` line when the ladder's thread does not come back. The service's instance word is the one this family was expected to
   announce (`0x101`); another instance of service 69 is now taken with a line saying so, rather than costing a boot.
   (needs-boot: §8.8 boot 6a) **Confirmed on the tablet** by the seventh round: buffers posted, hello, service, steps 502–508.
26. **P21: the firmware's router stopped sending after its tenth packet (ram-v32-probe 21:18, installed v3.2a 21:31,
   2026-11-12).** Both boots: steps 502–508 answered, then step 509 (the regulatory database, four segments) with three answers
   and no fourth; `9 requests, 8 responses, 2 indications, 4 download segments`, `11 packets out, 12 in`, `the router: 12 packets
   in, 10 data`, stopped after 10018 ms, stage 504. The firmware alive (no RDDM, no system error).
   **Evidence:** the firmware sent exactly ten data packets to our port (eight answers, two indications) and nothing after them,
   with its eleventh (the answer to the last segment, which completed on channel 20 like every other request) due; the `11
   packets out` are the hello, the nine requests and exactly one more, which can only have been the router's flow-control answer
   (the copied router sends no other packet on its own). Ten is the router protocol's high-water mark.
   **Mechanism (inferred from the protocol, reproduced by the host test):** a sender that sets `confirm_rx` counts what it has
   sent to one destination node:port, asks for the confirmation on the fifth packet and stops sending to that destination at the
   tenth until a RESUME_TX arrives whose payload names it (net/qrtr: `QRTR_TX_FLOW_HIGH` 10, `QRTR_TX_FLOW_LOW` 5; the resume is
   matched on the payload's client node and port, which the receiver fills with its *own* address). `package/PianoPower/QcomQrtr.c`,
   which `WifiQrtr.c` copies, answered with the *sender's* node and port in the payload, from the control port to the remote's
   control port: a resume of a flow the firmware does not have. The firmware therefore asked once (its fifth packet, the answer
   to the memory response), got nothing it recognised, and held its eleventh. The test's simulated firmware now does the
   protocol's flow control: with every answer lost it stops exactly where the tablet did (the same nine/eight/two/four counts,
   step 509 waiting on segment 4 of 4), the old answer resumes nothing, and the new one runs the stock files' sizes to the ready
   indication.
   **Ruled out (evidence):** the file and its message: `regdb_xiaomi.bin` (24278 bytes, the stock file Ubuntu ships as
   `regdb.bin`), kind 4, file identifier `0xff`, the remaining size in `0x11`, the end flag on the last segment — the form of
   Ubuntu's ath12k (read for facts), and the firmware accepted the first three segments of it with result 0 (a segment it rejects
   is answered with a failure, not with silence). The board identifier: the chip's capability gives `0xff` (no board in this unit's
   OTP) and Ubuntu's driver uses its default `0xff` the same way, then, with no board-2 container, its single API-1 board file —
   `bd_p81.elf`, which is what step 510 sends (no board data had been sent yet: the four segments were all the regulatory
   database's). TME-L (the security engine's patch, request `0x005b`, which another SM8750 tree sends before the regulatory
   database and whose stock answer is the refusal 83): Ubuntu's kernel for this tablet goes from the chip's capability straight to
   the regulatory database and its firmware lock (`piano-wlan.json`) has no TME-L file, and its Wi-Fi works — not needed
   (inference from Ubuntu's source and file set; a boot would show it if the firmware ever asked).
   **Changed:** the RESUME_TX goes from our port to the sender's node:port with our node and port in the payload (`WifiQrtr.c`'s
   "PianoWifi change", `UPSTREAM`); one the link refuses is owed and sent again on the next turn (`qcom_qrtr_poll`); the
   counters (`rx_confirms`, `resumes_sent`, `resume_owed`) in a `flow control:` line. Logging, so that the next stop names
   itself: every answer and indication with its result and error codes and the segment it answers, the board identifiers of
   both capabilities and the board file's reason, each download's file, size, segments and identifier, the unanswered request at
   a timeout, the ring report in three lines (one 300-byte held line had cut it), the `dma:` lines at a stop with a live chip.
   Behaviour aligned with Ubuntu on the way: the file identifier is the chip capability's board (0xff without one; the radio
   capability's is only noted), the board file's kind comes from its ELF magic, each request has its own 10 s (an answer
   restarts it), and the trace configuration (which Ubuntu's kernel may not send at all) no longer ends the level when refused or
   unanswered. **Also affected, not changed here:** PianoPower's own `QcomQrtr.c` (the DSP's link) has the same answer; whether a
   DSP service ever sets `confirm_rx` on that link was not checked (its owner's call: a service that does would go quiet after
   ten packets). (needs-boot: §8.8 boot 7a)

27. **P22: the firmware asserts around the virtual device and the scan (ESP v3.3d, 23:33, 2026-11-12).** The first boot that
   reached level 6: the transport up (2 credits of 2176 bytes, 5 endpoints, version 1; our endpoint 1 on pipes 3 out / 2 in),
   the message interface connected, the capability event and two extended ones, the initialisation accepted, the firmware's
   ready event at +829 ms (status 0, 16 peers, **1 extra address, its own address all zeros**). Then step 607 (the virtual
   device) at +829 ms, step 608 (the passive scan, 22 channels) at +929 ms, and at **+1045 ms** `the chip reported a system
   error or its crash dump environment`: EE RDDM, MHISTATUS `0000ff05` (SYSERR). The scan never started (0 scan events), 3
   commands and 10 events had gone by, and **3 events this level knows nothing about** were counted but not named.
   **What the crash dump gave (the P19 path working, its first real use):** 9 of 9 segments, a 12-entry header, and the
   firmware's own reason out of `Q6-SFR.bin` — `cmnos_assert_patched.c:604 ... line#488`. A firmware assert, not a bus fault:
   both ends' PCI status, the AER words, the SMMU's global fault and bank 80's FSR were clean, and the MSI sink had the chip's
   write.
   **Cause (inferred from Ubuntu's order, not yet proven on the tablet):** this level said nothing to the radio between the
   firmware's ready event and its first virtual device, where Ubuntu's driver, for a **single-radio** chip of this family
   (`wcn7860 hw2.0`: `single_pdev_only`), always sends first (a) the hardware mode (`WMI_PDEV_SET_HW_MODE`, "two bands at
   once", on its own and **not** inside the initialisation, which carries no mode structure for such a chip), (b) six radio
   parameters (protected-management-frame QoS on, dynamic bandwidth on, the ARP class override off, mesh multicast on, and the
   two chain masks), and (c) **the channel list** (`WMI_SCAN_CHAN_LIST`) built from the firmware's own regulatory event — a
   scan whose channels the radio has never been given is the most plausible assert of the three (§10.17 named exactly this
   risk). And (d) its virtual device never carries an **all-zero address**: the address comes from the firmware's ready event,
   and a host with none of its own makes a locally administered one rather than create a device with zeros, which is what this
   level did.
   **Changed (all four, in Ubuntu's order, behind one boot argument with sub-values, `piano-wifi-l6pre`):** bit 1 the hardware
   mode (and its answer's status in the log), bit 2 the six parameters, bit 4 the channel list — the firmware's regulatory
   event is read (both forms, with every rule's frequencies, bandwidth, power and flags) and turned into the channels its rules
   allow, each with its passive and radar bits; with no event this driver sends its own list of WifiHw.h — bit 8 a usable
   device address (the ready event's own, then the address its array carries, then `02:50:49:…` of piano's own; the address is
   never logged, only where it came from), bit 16 wait up to `piano-wifi-l6regwait` ms for the regulatory event first, bit 32
   pace the commands `piano-wifi-l6pace` ms apart so that a log names the last command before a death, bit 64 **no scan at
   all**: the device is created, the level holds `piano-wifi-l6hold` ms and tears down, which tells a death at the device from
   one at the scan. Default `0x3f` = 63 (everything but the last). Logging: the regulatory domain and its first eight rules, the
   channel list's counts (and what it left out), the hardware mode's answer, where the device's address came from, the step
   whose command went out last, and **the identifier of every event this level knows nothing about** (the first boot's three
   were a count alone).
   **Not changed, and the next suspects if the assert stays:** the data path. Ubuntu connects the firmware's data interface
   (HTT, service `0x0300`) over the transport **before** the message interface and sends its version request right after the
   initialisation; this level connects the message interface alone (§10.20). If `piano-wifi-l6pre=0x3f` still asserts, the
   order to try is `0x7f` (the same without the scan, to place the death) and then the HTT connection, which is level 8's
   first piece brought forward. (needs-boot: §8.8 boot 8a)

28. **P23: the scan ran and no frame reached the host (the SPMI-lock probe, 2026-11-13 02:05).** P22's set-up worked: the
   hardware mode accepted, 6 of 6 parameters, the regulatory event (country CN, 4 rules: 2402-2482, 5170-5250, 5250-5330 radar,
   5735-5835) turned into a 26-channel list, the virtual device created with piano's locally administered address, and a scan
   that **started, visited all 22 channels and completed** — with **0 management frames**, while two of the owner's networks
   were in range on both bands. 194 events, 3 of kinds this level did not know: `0x16005` (the firmware's memory-dump update),
   `0xb00b` and `0x1d021` (not in the chip's own host's list; neither can carry scan results).
   **Cause (inferred from the chip's own host, which says it in so many words):** on this chip family (`rx_mac_buf_ring`) the
   receive DMA takes every buffer it writes a frame into from rings the **host** provides — the host fills a host-to-firmware
   refill ring ("host fills rx buffer to fw and fw fills to rxbuf ring for each rxdma"), the firmware hands those buffers on to
   one buffer ring per radio (also host memory), and only then can any frame be received, management frames for the firmware
   included. This level gave it none, so the radio could tune, dwell and count but keep nothing. The other candidates do not
   fit: management frames do come as `WMI_MGMT_RX` on this family; the receive filter the host programs concerns data frames
   (management frames are not in it); the 2.4 GHz band's single rule is the whole band; the dwell times are the chip's host's.
   **Changed (`package/PianoWifi/WifiHtt.h`, behind `piano-wifi-l6pre` bit 7, on by default):** the firmware's data interface
   (HTT, transport service `0x0300` on pipes 4 out / 1 in, connected **before** the message interface, no credit flow) and its
   version exchange; the six rings the chip's own host announces with `HTT_SRING_SETUP` — the refill ring (511 buffers of 2
   KiB, host to firmware), the two radio buffer rings (empty: the firmware fills them), the receive DMA's error ring (its tail
   moved up to the hardware's head every turn), the two status rings (63 buffers each, given back as the hardware takes them)
   — all in the 32-bit window with every pointer in host memory. Pipe 4 gets its ring only with the bit, so a boot without it
   programs exactly what it did before. The buffers return to the firmware's own manager by default (bit 9: to "software ring
   1", as the chip's own host has them — whose release ring is level 8's and does not exist yet, hence not the default).
   **One boot, A/B:** the first scan runs as on 02:05; then the radio's statistics; then the version, the rings and a
   **second scan** with its own identifier; then the statistics again. **Diagnostics (bit 8, on by default):** every channel
   visit's noise floor and the busy and receiving shares of its cycles, the radio's receive counters after each round
   (statuses received, MPDUs delivered to the firmware and to the host, MPDU and PHY errors), and every event identifier with
   its count. Together they say which of three things is true: a radio that hears (busy and receiving cycles) but keeps nothing
   without buffers (round 1 empty, round 2 with networks: the cause above, confirmed); one that hears and still keeps nothing
   with them (the firmware's counters then say whether the frames reached it: the rest of the data path, level 8's REO and
   release rings, is next); or one that does not hear at all (no receiving cycles, an odd noise floor: the RF side, the board
   file or the calibration). **Also changed:** the scan list's log lines no longer print a network's name, only its length
   (with `piano-wifi-bssid=1` the whole name and address): the owner's networks must not reach a console log; the registry
   property, which `tooling/native/piano-wifi` reads on the tablet, keeps both. (needs-boot: §8.8 boot 9a)
   **Confirmed on the tablet (27.3 RAM boot 2026-11-13 10:40, `ram-k273-wifi-p23`):** round 1 empty as before; the data
   interface version 3.137, the six rings announced, the refill ring's 511 buffers taken by the firmware (its tail at 511), and
   **round 2 heard 29 management frames (28 beacons): 18 networks**, the owner's among them on both bands. One of the owner's
   networks was missing: the scan's static 22 channels leave out the radar ones. **P23b:** the scan now visits every channel of
   the list built from the firmware's regulatory event (CN: 26 channels, the four radar channels 5260-5320 among them, scanned
   passively as the whole scan is); the static table is only used when no event came.

## 11. Licence note

This repository's packages are BSD-2-Clause-Patent. Linux (`pcie-qcom`, `phy-qcom-qmp-pcie`, `pwrseq-qcom-wcn`, `mhi`, `qrtr`, ath12k,
mac80211) and SUN's kernel patches are GPL-2.0: specification only (behaviour, register offsets and bit positions as facts);
no code, comment or table is copied. Levels 5 and 6 rest on that rule more than any level before them: the message identifiers,
each field's tag and type, the order of the exchange, the ring and descriptor layouts and the tables of sections 12 and 14 are
wire facts read from the Ubuntu kernel (`yzddmr6/linux-sm8750-piano` `piano/dev` `7f8d6be`) and from the MHI and router headers,
and were written fresh here as writers, readers, step machines and tables of this repository's own shape;
`package/PianoWifi/UPSTREAM` lists each file that was read and for which fact. Level 6 added `htc.c`/`htc.h` (the transport's
header, its four messages, the credit records), `wmi.c`/`wmi.h` (the command and event identifiers, the TLV form, the resource
configuration's field order and this chip's values, the scan request's fields, the scan and management-frame events), `ce.c`,
`hal.c`, `wifi7/hal.c`, `wifi7/hal_wcn7850.c`, `wifi7/hal_qcc2072.c`, `wifi7/ce.c`, `wifi7/hw.c`, `hw.h` and `core.c` (the ring
registers and their units, the descriptor fields, the pointer units, the host and target pipe tables, the order the whole
start-up runs in) and `mac.c` (what a scan needs: a created virtual device, the dwell times, the flags) to that list — read for
those facts, nothing copied. Ath12kWifi is **not** used and no decision about it was made here: levels 1-6 need none of it. The QRTR codec
and the two SMMU pieces are copies of this repository's own BSD files, with `UPSTREAM` records and no change to the packages they
came from. The PHY init sequence comes from PaganiWifi (BSD) with attribution, not from mainline, and the
provenance question that leaves open is §10.4 and the header comment of `package/PianoWifi/WifiPhySeq.h`.
UniXNU-sheng and PaganiWifi (BSD-2-Clause-Patent) and this repository's own packages are reused with `UPSTREAM` records. Ath12kWifi
compiles GPL sources at build time; if the owner chooses it, it is consumed as an external, pinned build input and its kext is
not distributed. No Qualcomm binary is reverse engineered. Firmware (`LicenseRef-Proprietary-Firmware`, no redistribution) is read
at run time from the tablet's own `modem_a` (or a staged copy under the ignored `out/`), never committed; the MAC address is read
at run time and never logged or committed.

## 12. Level 5's message table

The WLAN firmware's service is 69 (`0x45`), version 1, instance 1 on this chip family; its announcement's instance word is
`version | instance << 8` = `0x101`, and it is reached at the node and port that announcement carries. Every message below is a
QMI message inside a router data packet: a 7-byte header (type, transaction, message id, payload length) and then TLVs (tag, a
16-bit length, the value). Scalars are little endian; a structure is its members in order with no padding; an array inside a TLV
begins with its element count (one byte here, two for the data of a download); a string TLV of its own carries only its characters.
A response always carries tag `0x02`: a 16-bit result then a 16-bit error, result 0 being success. The tags below are what this
driver writes and reads (`package/PianoWifi/WifiWlfw.h`, host-tested in `tests/qmi_host.cpp` against a simulated firmware).

One layer down, in the router that carries every message, flow control (P21, §10.26): the firmware's router sets `confirm_rx` on its fifth data packet to
our port and sends nothing more to it from its tenth until a RESUME_TX arrives that names our port. This side answers every
packet that asks, at once: a version 1 control packet of type 7 from our node and port (`1`, `0x4000`) to the sender's node and
port, its 20-byte payload the command (7) then **our** node and port. This side never sets `confirm_rx` itself, which the
firmware does not require. Every answer and indication is logged with its result and error codes (`answer to 0x…`,
`indication 0x…`), and a request still outstanding when its deadline passes is named (`unanswered: request …`).

| Step | Message | Id | Direction | What is in it |
|---|---|---|---|---|
| 502 | radio capability | `0x0057` | request / response | request: empty. Response: `0x10` radios (1), `0x11` board identifier (4), `0x13` single-chip multi-link (1). Optional: a refusal is not a failure |
| 503 | register for indications | `0x0020` | request / response | `0x10` firmware ready (1) = 1, `0x15` client identifier (4) = `0x4b4e454c`, `0x16` memory request (1) = 1, `0x17` memory ready (1) = 1, `0x18` initialisation done (1) = 1, `0x1b` calibration done (1) = 1. Response: `0x10` firmware status (8) |
| 504 | host capability | `0x0034` | request / response | `0x10` clients (4) = 1, `0x13` platform bits (1) = `0x0e` (the host's own C-states, the chip's internal sleep clock, a global PCIe reset), `0x14` board-file support (1) = 1, `0x16` PHY microcode support (1) = 1, `0x17` it is cached by the host (1) = 1, `0x1a` calibration done (1) = 0, `0x1c` memory configuration mode (1) = 0, `0x27` feature list (8) = `0x50` (no PERST# pull on reset, the auxiliary microcode) |
| 505 | the firmware asks for memory | `0x0035` | indication | `0x01`: a one-byte count, then per region: size (4), type (4), a one-byte count of configuration entries, then per entry offset (8), size (4), secure flag (1). Types: 1 host memory, 2 the board file's, 3 the PHY microcode's dump, 4 the calibration database, 8 multi-link global, 9 pageable, 0xb shared with the audio DSP |
| 506 | the addresses | `0x0036` | request / response | `0x01`: a one-byte count, then per region: address (8), size (4), type (4), restore (1) = 0. A count of 0 is the "ask again in smaller pieces" form, which the firmware answers with a failure and a fresh request. This driver answers types 1, 3, 4, 9 and 0xb with memory of its window and every other type with the address 0 |
| 507 | the memory is ready | `0x0037` | indication | empty |
| 508 | chip capability | `0x0024` | request / response | request: empty. Response: `0x10` {chip (4), family (4)}, `0x11` {board (4)}, `0x12` {system-on-chip (4)}, `0x13` {firmware version (4), build time as a counted string}, `0x14` build identifier (a string of its own), `0x15` radios (1), `0x16` millivolts (4), `0x17` timer frequency (4), `0x18` one-time-programmable version (4), `0x19` calibration read timeout (4), `0x1a` capabilities (8), `0x1b` chain capability (4), `0x1c` four {start (8), size (8)} device-memory entries |
| 509, 510 | a file in segments | `0x0025` | request / response | `0x01` valid (1) = 1, `0x10` file identifier (4) = the board of the chip capability's `0x11` (`0xff` when it gives none; the radio capability's board is not used), `0x11` what is still to come including this segment (4), `0x12` segment number (4), `0x13` the data (a two-byte count then at most 6144 bytes), `0x14` last segment (1), `0x15` kind (1): 4 the regulatory database (`regdb_xiaomi.bin`, 24278 bytes: 4 segments), 1 an ELF board file or 0 one that is not (from the file's first four bytes; `bd_p81.elf` is an ELF, 125272 bytes: 21 segments, capped at 256 KiB - this chip's board-file size). One answer per segment. This unit's chip gives board `0xff` (no board in its OTP): with no board-2 container in the stock image, the board file is the single API-1 one, `bd_p81.elf` (the choice the chip's own driver makes, §10.26) |
| 511 | the PHY microcode | `0x003c` | request / response | `0x01` address (8), `0x02` size (4): `phy_ucode20.elf` read into the window |
| 512 | the auxiliary microcode | `0x005a` | request / response | the same two tags: `aux_ucode20.elf`. This chip's entry asks for it; the WCN7850's does not |
| 513 | the trace configuration | `0x0044` | request / response | `0x10` what is still to come (4), `0x11` segment number (4), `0x12` the data (a two-byte count then at most 6144 bytes), `0x13` last segment (1): `qdss_trace_config_v2.cfg`. This chip does not set the "configuration missing" feature bit, so it is sent; a refusal, no answer within the request's deadline, or no such file is logged and the client goes on to wait for the ready indication |
| 514 | the firmware is ready | `0x0038` | indication | empty. **Level 5's gate** |
| 515 | the firmware's options | `0x002f` | request / response | `0x10` firmware logging (1) = 1 (only with `piano-wifi-mode=1`) |
| 516 | the host's configuration | `0x0023` | request / response | `0x10` host version (a string of its own) = `WIN`, `0x11` the copy engine's pipes: a count then per pipe {number, direction, entries, largest transfer, flags} (nine of them, below), `0x12` the services' pipes: a count then per entry {service, direction, pipe} (fourteen, below), `0x17` the shadow registers: a count then one 32-bit address each (40 with `piano-wifi-shadow=1`, none otherwise) |
| 517 | the mode | `0x0022` | request / response | `0x01` mode (4) = 0 (normal; 4 is off), `0x10` hardware debugging (1) = 0 |

The nine pipes and the fourteen service entries this driver sends are the chip's own (`package/PianoWifi/WifiHw.h`, facts):
pipes 0 out and 1 in carry the transport's control and the data messages back (32 entries of 2048 bytes), 2 in and 3 out the
firmware's and the host's messages, 4 out the host's data (256 entries of 256 bytes, no interrupt), 5 in the packet log, 6 the
firmware's own copies both ways (16 KiB), 7 host to host, 8 the data accelerator's; the services map the control service to pipes
0 and 2, the firmware's four traffic classes and its control service to pipes 3 and 2, and the host's data service to pipes 4
and 1. The shadow registers are 40 words from BAR0 + `0x8fc`, each standing for one ring's head pointer (a destination ring's tail
pointer is its head pointer's offset plus four); the list is built from the register map of the rings level 8's data path will
create, which is why `piano-wifi-shadow` is off by default and why level 6 writes the real registers through the window (§10.18).

## 13. Level 6 as built, and what levels 7 and 8 need

Level 6 is the host transport and the firmware's own message interface on top of the copy engine, and the scan that proves both.
It is the first level with rings of its own in host memory — which is why section 10.11's 32-bit window is settled at level 5.
The pieces as they are written (`WifiCe.h`, `WifiHtc.h`, `WifiWmi.h`, `WifiLink.h`; host-tested in `tests/wmi_host.cpp` against a
model of the copy engine's registers and a firmware that speaks both protocols):

1. **The copy engine's rings.** Four of the nine pipes section 12 describes, which are all the transport needs: 0 out (the
   transport's own control messages), 3 out (the host's messages), 2 in (the firmware's messages and the transport's answers),
   and 1 in (the firmware's data messages — unused before level 8, but given a ring and buffers so that a copy cannot stall).
   Per pipe a **source ring** (the host writes a descriptor naming a buffer to send), a **destination ring** (the host writes a
   descriptor naming an empty buffer; it is a producer ring of the host's just like the source ring) and a
   **destination-status ring** (the chip writes one descriptor per buffer it filled, with the length). Both pointers of every
   ring count 32-bit words. The host's pointer lives in the chip's register space (the units at `0x01b80000` and up, through
   BAR0's window register); the chip's lives in the window, in a block of its own, at an address the host programs into the
   ring — so a poll reads host memory and never a register. No MSI, no interrupt threshold: one thread polls (§10.18).
2. **The host transport.** The firmware speaks first: a "ready" message on endpoint 0 with its credit count and credit size.
   Then one service is connected — the firmware's message interface, `0x0100` — and its answer gives the endpoint number and the
   largest message it takes; the pipes are **not** negotiated, they are the service table level 5 already sent. Then "set-up
   complete". The firmware has nine endpoints and may name any of them, the last included. Every message after that carries
   the 8-byte header of section 14, and every message on that endpoint costs
   credits, which the firmware gives back in the trailer of what it sends (a credit wait is not a failure; the step's deadline
   decides).
3. **The firmware's message interface.** Its service-ready event says how many radio chains and scan channels it has and how
   much host memory it wants (none on this chip: §10.16), and one or two **extended** capability events follow it — which its
   own host waits for before it says anything, and so does this driver (half a second at most, or less when the second of them
   arrives). The host then answers with the initialisation command: the 72-word resource
   configuration of this chip's own entry (four virtual devices, sixteen peers, thirty-two traffic identifiers, native-Wi-Fi
   decapsulation, the two flag words its own host always sends) and an empty memory-chunk array. The firmware then sends its
   ready event, with its own MAC address and its status.
4. **A station virtual device.** One, created and never started: a scan does not need a started device on this chip family
   (its own driver defers the start to the association). It carries one stream entry per band.
5. **A passive scan.** The channel list of `WifiHw.h` (the 2.4 GHz band's 13 channels and the 5 GHz band's 9 non-radar ones;
   `piano-wifi-band=0` leaves the upper band out), the dwell times its own host uses, and the passive flag. The firmware answers
   with a "started" event, then one management-frame event per beacon or probe answer — each with the frame, its channel and its
   signal strength — and finally "completed". A frame whose own checks failed (the status word's check, decryption or key bits)
   is thrown away whole: a network's address must never come from one. The rest are parsed where a frame from the air must be:
   the third address is the network's, element 0 its name and element 3 its channel, and an element that runs past the frame
   ends the walk. The list keeps at most 32 networks, merges repeats (the strongest signal wins) and counts what it dropped.
6. **The teardown.** The scan stopped if it still runs, the virtual device deleted, the chip's completions waited for, and only
   then the rings disabled — in that order (§8.7).
7. **What level 6 does not do.** No association, no keys, no data path: those are levels 7 and 8.

**Level 7 (association), what it needs**: the firmware's regulatory event read and the channel list sent
(`WMI_SCAN_CHAN_LIST`, §10.17 — **built since P22**, §10.27: level 6 now sends it before its own scan, so level 7 inherits
both the event's rules and the list); the virtual device started on the owner's channel; a peer created for the access point;
the four-way handshake, with the keys installed by `WMI_VDEV_INSTALL_KEY`; and the management frames the handshake needs sent
through `WMI_MGMT_TX`. The wire side of all of that is built and host-tested (`package/PianoWifi/WifiAssoc.h`,
`tests/assoc_host.cpp`): the regulatory rules and the channel list, `WMI_VDEV_START`/`UP`/`STOP`/`DOWN` with the channel
structure, `WMI_PEER_CREATE`/`DELETE`/`SET_PARAM`, `WMI_PEER_ASSOC` with the peer's flags and legacy rates,
`WMI_VDEV_INSTALL_KEY` for a pairwise and a group key, and `WMI_MGMT_TX` with the frame in the 32-bit window. What is not
built yet is the step machine that drives them (`piano-wifi=7`), the supplicant's four-way handshake and the path the
passphrase takes — §13.2 is the plan, and none of it may run before level 6 passes on the tablet. **Level 8 (traffic)**: the
data path's rings (the transmit-command, reorder and buffer-manager units whose head pointers §12's shadow list already names),
the firmware's data interface (HTT) connected as a second transport service, and the receive and transmit paths wired to the
Ethernet interface below — which is where `en1` gains its link.

Expected effort: 4-8 weeks each written fresh, or 1-2 weeks of board glue if the owner decides for Ath12kWifi (section 10.8 —
the decision is still open, and nothing in levels 1-6 depends on it).

### 13.1 The macOS side: `en1` as built, and the association design

The station the levels above build has to appear to macOS as something it can route over, and section 5.2 settles what that is:
**an Ethernet-like interface**, not a Wi-Fi one. What is built, and what is still design:

1. **Built**: `package/PianoWifi/PianoWifiEthernet.cpp`, an `IOEthernetController` publishing `en1` with `IOBuiltin` and without
   `IOPrimaryInterface` — exactly the shape `PianoNullEthernet` has in the collection, and its vtable check passes the same way.
   Its link status is **valid but not active** (macOS shows the interface down and routes nothing over it), every frame handed
   to it is dropped and counted, and it carries level 6's scan list as the property `piano-wifi-scan` (one dictionary per
   network: name, address, channel, frequency, signal strength, beacon and probe counts, and whether the network says its name).
   It owns no hardware: its personality matches the resource `PianoWifiLink`, which PianoWifi publishes only when level 6 came
   up, so `piano-wifi=0..5` leaves macOS with `en0` alone.
2. **Built**: `tooling/native/piano-wifi` on the tablet — `piano-wifi` prints the table with every address masked to its first
   three bytes, `--full` prints them whole, `status` prints what the ladder reached and whether `en1` is there, `--raw` the
   property as `ioreg` gives it. It reads the registry and nothing else.
3. The interface's address: this unit's own is in the `persist` partition and is read by the same read-only path levels 4 and 5
   use for `modem_a` — **still design**. Until then `en1` takes a locally administered address of piano's own (not the unit's),
   or the one the firmware's ready event reports when that is not all zeros (the property `piano-wifi-mac`). Neither is ever
   logged whole or committed (section 11).
4. **Design — the association path.** A kernel control socket (`kern_control`, a name of this driver's) is the control endpoint:
   a user-space supplicant opens it, reads the scan list, and asks for a join with a profile (the network's name, its address,
   the security it wants). WPA2-PSK and WPA3-SAE both run their handshakes **in user space** — wpa_supplicant 2.12 is
   BSD-licensed, and its `driver_*.c` shape is what the control messages mirror — so the key material never enters this
   repository and never enters a log: the kernel side is given the pairwise and group keys only as opaque bytes to install, and
   the driver logs that a key of N bytes was installed for a peer, never the bytes. The profile lives under
   `/private/var/db/piano-wifi/` (mode 600, never in a repository), and the CLI gains `join NAME`, `leave` and `status` over the
   same socket. **How the passphrase gets there**: it exists on the Mac only, in a mode-600 file outside every repository; the Mac
   copies it over the USB network (the `desk-ssh` path, standard input, never an argument, never echoed) into
   `/private/var/db/piano-wifi/` on the tablet, owned by root, the directory mode 700 and the file 600, and the supplicant reads
   it there at run time. It never goes into a boot argument (the command line is in every console log and in the ADT), an image,
   the console, a log line, a commit or a document; the network's name is a run-time argument of `join` for the same reason (the
   home network's names are private: they are not written here). The first association test prefers the 5 GHz network. What the kernel side owes the supplicant: the scan list, the management frames of the handshake in both
   directions, the association state, and the keys' installation — nothing more.
5. `en0` stays `PianoNullEthernet` until the clock identity question of section 5.3 is answered: TimeSync derives its identity
   from a built-in interface's MAC at its start, and this interface appears seconds later (level 6 takes tens of seconds). Once
   a boot shows the identity stays stable, the Wi-Fi controller can be published early (link down) as `en0` and
   PianoNullEthernet dropped.
6. No Wi-Fi menu, no AirDrop, no location by Wi-Fi: section 5.2 is where that trade is written down, and the USB-C network
   (section 5.2 C) stays the first network while this is built.

### 13.2 Level 7 as planned: the step machine, the handshake, and how the passphrase gets in

What is **built** (host-tested, inert: no boot argument reaches it yet) is the wire side in `package/PianoWifi/WifiAssoc.h`
and its facts in `WifiHw.h`. What follows is the plan for the rest, in the order it will be built, and the one question the
owner has to answer (the last paragraph).

1. **The step machine (`piano-wifi=7`, steps 701-711).** After level 6's scan: the regulatory event and the channel list are
   already sent (level 6 does it since §10.27); a **targeted scan** of the chosen network's channel refreshes its beacon and
   keeps its elements (the RSN element decides what the handshake must be); `WMI_VDEV_START` on that channel and its start
   response; `WMI_PEER_CREATE` for the access point; open authentication and the association request through `WMI_MGMT_TX`
   with their answers read from the same management-frame events level 6 already parses; `WMI_PEER_ASSOC` with the peer's
   capability, its legacy rates and the four-way-handshake flags; `WMI_VDEV_UP` with the network's address and the
   association identifier. Every step keeps level 6's shape: one command outstanding, a deadline each, a bounded poll, no
   allocation, the chip's health read every turn, and a teardown that deletes the peer and takes the device down and out.
2. **The four-way handshake, in the kernel, for WPA2-PSK with CCMP first.** The key material never leaves the kernel and
   never enters a log: the pairwise key is derived (PBKDF2-SHA1 over the passphrase and the network's name for the pairwise
   master key, the SHA-1 pseudo-random function over the two addresses and the two nonces for the transient key), the
   EAPOL-Key frames are built and checked (HMAC-SHA1 over the frame with the key-confirmation key), the group key is
   unwrapped with AES key wrap, and both are installed with `WMI_VDEV_INSTALL_KEY` (pairwise first, then the group key, then
   the peer authorised with `WMI_PEER_SET_PARAM`). The crypto is written fresh in this repository (`WifiSha256.h` is the
   precedent); SHA-1, its HMAC, PBKDF2 and AES key unwrap are a few hundred lines and are host-tested against the published
   test vectors of the standards, which carry no licence. **WPA3-SAE is not in the first version**: its handshake needs
   elliptic-curve arithmetic over P-256, which is a package of its own; a WPA3-only network is reported as unsupported
   rather than half-tried, and a transition network is joined with PSK.
3. **How the passphrase reaches the kext — the proposal.** The owner's network name and passphrase are a local secret: they
   must never be a boot argument (the command line is in every console log and in the ADT), never a log line, never in this
   repository or a document. The path:
   - a file on the installed Data store, `/private/var/db/piano-wifi/profile` (owner root, directory mode 700, file 600),
     written once by the owner (or copied over the USB network from the Mac, standard input, never an argument);
   - a tiny launch daemon (`com.unixnu.piano-wifi-join`, root, `RunAtLoad`) reads it and writes it **once** into a
     write-only sysctl, `machdep.piano_wifi_join` (`CTLFLAG_WR | CTLFLAG_ANYBODY` off: root only, and a read returns
     nothing but `set` or `not set`); the daemon then exits, so the secret lives in one process for milliseconds;
   - the kext keeps the bytes in wired kernel memory, derives the pairwise master key once, and **zeroes the passphrase**;
     the log only ever says `profile: a network name of N bytes and a passphrase of M bytes (set)`, never a byte of either.
     The network's name is also a secret here (it names the owner's home), so the log prints its length and never its text;
     the registry property keeps it no more than level 6's scan list already does.
   - a boot argument switches the **feature** on (`piano-wifi=7`) and nothing else; with no profile set, level 7 stops after
     the association request's refusal or simply holds, and says `no profile is set`.
   The alternative, an `IOUserClient` with a command to hand the bytes over, buys a cleaner interface (and a place for
   `join` / `leave` / `status`) at the cost of a user client and its entitlement checks in the first version. The sysctl is
   the smaller step and is reversible: `tooling/native/piano-wifi join` can be taught either.
   **Built (2026-11-13), the path itself:** `package/PianoWifi/WifiProfile.h` (the profile's structure and its rules, shared by
   both ends, host-tested in `tests/profile_host.cpp`), the sysctl `machdep.piano_wifi_join` in PianoWifi (registered from
   `piano-wifi=6`; a write needs root and exactly one profile; a read returns `not set` or `set (generation N): a network name
   of N byte(s), WPA2-PSK with a passphrase, the access point given/not given, the station's address given/not given`; the
   profile is wiped when the radio stops), and the userland unit `piano-wifi-join` (`configs/userland.toml`):
   `/usr/local/libexec/piano-wifi-join`, run once at boot by `com.unixnu.piano-wifi-join`, which refuses a profile file that is
   not root's with mode 600 in a directory of root's with mode 700, reads it, hands it over and wipes its copy; its log
   (`/private/var/log/piano-wifi-join.log`) and the kernel's line carry lengths only. **The file the owner writes**
   (`/private/var/db/piano-wifi/profile`, `chmod 700` the directory and `600` the file, `chown root:wheel` both):

   ```
   ssid=<the network's name>
   security=wpa2-psk
   psk=<the passphrase>
   # optional: bssid=<the access point's address, aa:bb:cc:dd:ee:ff>
   # optional: mac=<the station's own address, locally administered: 02:…>
   ```

   Nothing reads the profile yet: level 7's step machine is the next piece, and it waits for level 6 to deliver beacons
   (P23).
4. **What stays out of the first version:** roaming, power save (the radio stays awake: `WMI_STA_POWERSAVE_MODE` off),
   protected management frames beyond what the access point requires, and the data path. Without level 8 an association
   gives `en1` a link that carries no traffic yet — which is still the gate this level has to pass: the firmware's
   `WMI_VDEV_UP` accepted, the keys installed, and the access point's own log showing the tablet joined.

### 13.3 Level 7 as built: the join (2026-11-13)

`piano-wifi=7` runs level 6 as before up to its second scan; there the ladder keeps its virtual device and hands the transport
over (`step 629: the scan is done: the virtual device stays for level 7`) instead of tearing them down, and level 7
(`package/PianoWifi/WifiSta.h`, steps 701-717, host-tested in `tests/sta_host.cpp`) joins on them. Every event of the message
interface goes to the join first (`WifiLink.h` hook); every command it builds goes out on level 6's endpoint (`linkSendWmi`),
one at a time and paced 20 ms. Its order is this chip's own (`wcn7860 hw2.0`: `vdev_start_delay`, read from Ubuntu's ath12k for
behaviour, `UPSTREAM`):

| Step | What | Answer waited for | Fails with |
|---|---|---|---|
| 701 | the join profile (`machdep.piano_wifi_join`, §13.2), up to `piano-wifi-l7wait` ms | the sysctl written | `no join profile was set` |
| 702 | the network: the scan list's entries whose name is the profile's byte for byte; the profile's access point if it names one in the list, else the strongest, a 5 GHz one first (bit 3) unless weaker than -82 dBm; a radar channel only with bit 4 (then joined passively). The security must fit: an open profile an open network; WPA2-PSK an RSN element with PSK and CCMP and no required management frame protection | — | `not in the scan list` (after 703), `security ... not supported` |
| 703 | a passive scan of our own over the regulatory list (id `0xa010` + n), at most `piano-wifi-l7scans` | its completion | — |
| 704 | the station's parameters (bit 0): `WMI_VDEV_SET_PARAM` transmit encapsulation and receive decapsulation 2 (802.3; bit 6: 1, native Wi-Fi), the stream count; `WMI_STA_POWERSAVE_PARAM` receive wake policy 0 (wake), transmit wake threshold 1 (always), PS-Poll count 0 (no limit); `WMI_STA_POWERSAVE_MODE` 0 (off) | — | — |
| 705 | `WMI_PEER_CREATE` for the access point | — | — |
| 706 | the HTT peer map naming it (bit 2; types 3, 0x1e or 0x2b on the data interface) | the map, `piano-wifi-l7step` ms | goes on, logged |
| 707 | its data set-up (bit 1): `WMI_PEER_SET_PARAM` 19 default routing = 3 (hash steering, receive ring 1), then `WMI_PEER_REORDER_QUEUE_SETUP` (`0x6013`) for traffic identifiers 0-16, each a 128-byte-aligned queue descriptor in the window (512 bytes for 0-15, 128 for 16), window 1 | — | — |
| 708 | `WMI_VDEV_START` on the network's frequency: 11a or 11g (legacy only), its beacon interval and DTIM period, passive on a passive channel | `WMI_VDEV_START_RESP` | `refused to start the device` (status) / timeout |
| 709 | Open System authentication (`WMI_MGMT_TX`, the frame in the window), up to `piano-wifi-l7tries` tries of `piano-wifi-l7frame` ms | the access point's answer as `WMI_MGMT_RX` (sequence 2): its status | `refused the authentication`, `did not answer` |
| 710 | the association request: capability (ESS, privacy, 2.4 GHz short preamble and slot, spectrum management when asked), listen interval 5, the name, the rates (supported + extended), the power capability and supported channels when spectrum management is asked, the RSN element (version 1, the network's group cipher, CCMP, PSK, no capabilities) | the association answer: status and AID | `refused the association` (status), `did not answer` |
| 711 | `WMI_PEER_ASSOC`: AID, the answer's capability, the legacy rates in the firmware's form, 11a/11g, WPA2: need-PTK-4-way (and no authorisation) | `WMI_PEER_ASSOC_CONF` | goes on, logged |
| 712 | `WMI_VDEV_UP` with the AID and the network's address | — | — |
| 713 | an open network's peer authorised (`WMI_PEER_SET_PARAM` 3 = 1); WPA2 waits for the keys (piece 3) | — | — |
| 714 | associated, watched for `piano-wifi-l7hold` ms: a deauthentication or disassociation from the access point (to us or broadcast), the firmware's kickout, a roam event with "beacon miss" end it, each logged with its reason | — | — (a disconnect is logged, not a failure) |
| 715 | the teardown: our deauthentication (bit 5, reason 3; only while up and not already ended), `WMI_VDEV_DOWN`, `WMI_PEER_DELETE` (its answer waited for), `WMI_VDEV_STOP` (its answer waited for); then level 6's own teardown deletes the device and disables the rings | `PEER_DELETE_RESP`, `VDEV_STOPPED` (logged when absent) | — |

**Since boot 10a (2026-11-13):** a network's frequency is its own channel's (the DS parameter set, else the HT operation's
primary channel, else the header's channel number), not the frequency the radio was tuned to when it heard the frame; every
access point of the name is a **candidate**, tried in turn (bit 10): 5 GHz not radar, then radar, then 2.4 GHz, then 5 GHz
weaker than -82 dBm, the strongest first within a class, the profile's access point first; a candidate whose start is refused,
whose authentication or association is not answered or refused, is taken down (peer deleted, device stopped) and the next one
joined from its peer, each with its class and result in the log. Level 7's own scans begin with **directed probes** (bit 9):
an active scan naming the profile's network on the 5 GHz channels the station may transmit on (not passive), with the band's
eight OFDM rates, then the same on 2.4 GHz with its CCK and OFDM rates, then a passive scan over every channel; a network that
hides its name answers only those, and its probe answer names the list's nameless entry. Every management frame and its
command are **checked** before they go out (`StaFrameCheck`: the three addresses, the type, the flags, the sequence, the
command's device, frequency field 0, no transmit parameters, the inline copy, the window address and length), logged as
booleans. The station's parameters gain the radio's **transmit power limits** for both bands (bit 8; `WMI_PDEV_SET_PARAM` 3 and
4 = 30 half-dB with the SAR reason, as this chip's host sends them). Checked against Linux for behaviour: the management frame
command's frequency is 0 (the device's own channel) and its transmit parameters off, as ath12k sends them outside a
remain-on-channel; a station's start on a radar channel keeps the passive flag (cfg80211 lifts "no initiating radiation" on a
beacon only in a world-roaming domain, and this unit's is CN), and its power fields are the channel's maximum in whole dBm
while the scan's channel list carries half-dB; neither differed from what level 7 sends. The profile is also read by the kext
from its root-only file while level 7 waits (`WifiProfileFile.c`, every 2 s, the helper's checks: the directory root's with
mode 700, the file root's with mode 600, at most 4 KiB, not a link; `piano-wifi-l7file=0` for the sysctl only).

What the log says (`level 7: ...` lines): the options, the network's list index, how many access points carry the name, its
frequency, channel, band, passive/radar, signal, beacon interval, DTIM, rate count and RSN suites as numbers; the profile's
hand-over (the name's length, the security); the peer map's peer identifiers; each answer's status code (with the standard's
name), the AID, each frame's completion status; how the association ended and its reason; the teardown's pieces. **Never** the
network's name, an access point's address, a passphrase or a key. `piano-wifi-level7` (registry) has the end state.

The address: en1 (`PianoWifiEthernet`) now takes the virtual device's own address (`piano-wifi-mac`, published by level 6
whatever it came from): before, the device was created with piano's `...:4e:01` and en1 kept `...:4e:03`, so a level 8 frame
from en1 would have carried a source address the firmware does not know. A profile that gives `mac=` and is already set when
level 6 creates its device gives the device that address.

**What a WPA2 join does without piece 3:** the association succeeds (status 0, an AID), the access point sends the first
EAPOL-Key frame as data, nothing answers it (no data path yet, no supplicant), and after its handshake timeout (a few seconds)
the access point sends a deauthentication or disassociation, typically reason 15 ("the four-way handshake timed out") or 2;
level 7 logs it and tears down. Data frames between the association and that end meet a receive path whose reorder rings are
not programmed yet (level 8): that is the probe's main risk (§8.8 boot 10a, "a crash after 710").

**The boundary for piece 3 (user-space key management):** the kernel side will deliver received 802.1X (EAPOL) frames, accept
802.1X frames to send and keys to install, and authorise the peer once the pairwise and group keys are in (`WMI_VDEV_INSTALL_KEY`,
then `WMI_PEER_SET_PARAM` 3); that interface lives with the data path (level 8), which is what carries EAPOL in both directions.

### 13.4 Status of levels 7 and 8 (2026-11-13, updated 2026-11-14 after boot 10a)

**2026-11-14:** boot 10a's two failures are understood and fixed (§8.8 after 10a: the off-channel frequency; the hidden
name); the candidates, directed probes, frame checks, power limits and the kext's own read of the profile file are in, host
tested, and probe `ram-k273-wifi-10b` carries them (§8.8 boot 10b). The parts below are the state of 2026-11-13.

**Landed (branch commits, host-tested, not yet run on the tablet):**
- Piece 1, the join (`piano-wifi=7`, §13.3): `WifiSta.h`, `WifiDot11.h`, the level 7 parts of `WifiAssoc.h`, `WifiHtt.h`
  (peer maps), `WifiWmi.h` (the scan entry's raw name, rates, RSN suites, beacon interval, DTIM), `WifiLink.h` (the hand-off
  at step 629, the event hook, `linkSendWmi`, the profile's station address), `PianoWifi.cpp` (level 7, its arguments, its
  deadline, the fast teardown on a suspend, `piano-wifi-level7`), en1 on the virtual device's address (`PianoWifiEthernet.cpp`).
- The profile file's parser shared by the helper and the kext (`WifiProfile.h` `piano_wifi_profile_parse`).
- The kexts build (`tooling/k27/build-native-piano-wifi.sh`, `build-native-piano-wifi-ethernet.sh`: objects and the import
  check, 344 kernel imports, no new library).

**Host-tested:** `tests/sta_host.cpp` (the frames byte for byte, the answers, the RSN parse, the reorder queue descriptors, the
peer maps, the choice of network, and the whole join against a firmware and access-point model: WPA2 and open, the command
order, every field the firmware reads, a transport that makes every command wait, no answer, refusals of the start, the
authentication and the association, a late profile, no profile, a network found only by its own scan, no peer map, a
kickout, a disconnect, a stop, the dry run, the A/B switches), `tests/wmi_host.cpp` (the hand-off: device kept, hook, a command
through `linkSendWmi`, the teardown after), `tests/profile_host.cpp` (the parser); every older test unchanged and passing.

**Still open:**
- **The profile on a RAM probe.** The helper `piano-wifi-join` (configs/userland.toml) was added after the 27.3 store seed was
  made (helper 2026-11-13 09:52, seed 01:42), so a probe built on that seed most likely has no `/usr/local/libexec/piano-wifi-join`.
  Two ways: (a) the kext reads the root-only file itself while level 7 waits (the parser is in; the read through the vnode
  KPI, as PianoPower's `PianoAdspFirmware.c` reads files, is **not written yet**); (b) write the sysctl with a helper copied to
  the tablet. The sysctl's input is one `struct piano_wifi_profile` of **124 bytes**, little endian: magic `0x314a5750` at 0,
  version 1 at 4, flags at 8 (bit 0 access point given, bit 1 station address given), security at 12 (0 open, 1 WPA2-PSK),
  name length at 13, passphrase length at 14, a zero byte at 15, the name at 16 (32 bytes), the access point at 48 (6), the
  station address at 54 (6), the passphrase at 60 (64); written with `sysctlbyname("machdep.piano_wifi_join", NULL, NULL, &p,
  124)` as root, read back as a status line of lengths only.
- **The 27.3 collection with level 7 links** (`tooling/k273/build-k273-image.sh`, the 0066 kernel, 2026-11-13 12:10: 26 native
  kexts, none from outside this repository; PianoWifiEthernet's vtable check 0 problem slots; `out/k27/kc-k27-native.bin` of
  this branch's worktree, 114212864 bytes, sha256 `5a8ca2a4...`). That script's own RAM image (the Turnip-staged 27.3 root
  seed) came out at 251961344 bytes and was refused by the 250 MB rule, as expected for that seed: it is not the probe.
- **The probe image** (§8.8 boot 10a): not built. The recipe is boot 9a's (`/tmp/a18-build-probe.sh` in the main agent's notes:
  the installed 27.3 entry's line with `data-persist=0`, `PIANO_EXTRA_ARGS="piano-reset-after=900 piano-reset-to=bootloader"`,
  the store seed and its trust cache, the 27.3 collection with the 0066 kernel) with `piano-wifi=6` replaced by `piano-wifi=7`
  in the line's driver arguments (they move into `/chosen/piano-boot-args-ram`).
- **Piece 2, the data path (level 8):** not started in code. Gathered so far (facts of Ubuntu's ath12k for WCN7860, which
  takes QCC2072's HAL and registers): the UMAC units (REO `0xa38000`, TCL `0xa44000`, WBM `0xa34000`), QCC2072's R0 offsets
  (TCL1 ring base `0x918`, REO1 `0x500`, REO2SW0 exception `0x8c0`, WBM idle link `0xd3c`, WBM2SW0 release `0xe08`, WBM2SW1
  `0xe80`, the TCL bank register `0xa4408c`) and the common R2 offsets (TCL1 HP `0x2000`, REO1 HP `0x3048`, REO2SW0 `0x3088`,
  idle link `0x30b8`, WBM2SW0 `0x30c8`), the register sequence of a ring (the copy engine's, which level 6 already drives),
  the ring identifiers (REO2SW1 1, SW2TCL1 24, WBM idle link 120, WBM2SW0 128, WBM2SW3 131 the receive error ring), the
  transmit-ring map (TCL 0 -> WBM2SW0, RBM SW0) and the receive buffers' manager (software ring 1). Still to read: the TCL
  data command and bank fields, the WBM release entry, the REO destination entry, the QCC2072 receive descriptor, the REO
  general/routing registers, the idle link list, the HTT receive-ring selection.
- **Piece 3's boundary** (EAPOL in and out, keys in, the peer authorised): rides on piece 2; not built.

**What the next 27.3 boot (10a) must answer, with its log lines** (the full table is §8.8 boot 10a):
1. Level 6 unchanged: `level 6: scan round 2 (with the receive buffers): ... K network(s)` with K >= 1.
2. The hand-off: `step 629: the scan is done: the virtual device stays for level 7`; no `the teardown:` line of level 6 yet.
3. en1's address: `PianoWifiEthernet: the interface takes the virtual device's address (its first three bytes 02:50:49)`;
   on the tablet `ifconfig en1 | grep ether` gives `02:50:49:41:4e:01`.
4. Level 7 starts: `stage 700: level 7: the join on level 6's virtual device`, `level 7: piano-wifi-l7=0x3f ...`,
   `step 701: waiting for the join profile`.
5. The profile: `join profile set (generation 1): a network name of N byte(s), WPA2-PSK with a passphrase ...` then
   `level 7: the join profile was handed over after N ms: a network name of N byte(s), WPA2-PSK ...`.
6. The choice: `step 702: the network`, `level 7: the network: entry E of the scan list (C access point(s) carry the profile's
   name, ...): F MHz, channel X (5 GHz), signal S dBm`, `level 7: its elements: ... RSN (group 4, pairwise 0x10, key management
   0x4, ...)`.
7. The set-up: `step 704`, `step 705: the peer`, `step 706: the peer's map` with `level 7: the data interface named the peer:
   peer P, hardware peer H`, `step 707: the peer's data set-up`.
8. The start: `step 708: the device started`, `level 7: the device's start response: status 0 (started) after N ms`.
9. Authentication: `step 709: authentication`, `level 7: authentication: try 1 of 3`, `level 7: frame 1: acknowledged
   (completion status 0)`, `level 7: the authentication answer: status 0 (success) after N ms`.
10. **The gate:** `step 710: the association`, `level 7: frame 2: acknowledged ...`, **`level 7: the association answer:
    status 0 (success), AID A, after N ms`**.
11. After it: `step 711: the peer's association` with `level 7: the peer's association confirmed after N ms`, `step 712: the
    station up`, `step 714: associated`; then (WPA2, no keys yet) `level 7: the association ended: a deauthentication (or a
    disassociation), reason R (...), N ms after the station came up`.
12. The teardown: `step 715: the teardown`, `level 7: the teardown: the station down`, `... the peer deleted (answered)`,
    `... the device stopped (answered)`, `step 716: done`, `level 7: done after N ms ...`, `level 6: the teardown: ... the
    virtual device deleted, the rings disabled`, `stage 718: level 7: the join ran to its end`; no `crash:` line.
A crash after step 710 is the unprogrammed receive rings meeting the first data frame (level 8 fixes it; `piano-wifi-l7=61`
is the control without the peer data set-up). Each other failure's meaning is in §8.8 boot 10a's last column.

## 14. Level 6's message table

Three protocols, one on top of the other, all of them little endian. The copy engine's descriptors and ring registers are in
`package/PianoWifi/WifiHw.h` with their units; what follows is what goes **on** the pipes (host-tested in `tests/wmi_host.cpp`).

**The host transport (HTC).** Every message carries an 8-byte header: word 0 is the endpoint (7:0), the flags (15:8) and the
payload's length (31:16); word 1 is the trailer's length (7:0) and a sequence number (15:8). Flag `0x01` asks the firmware for a
credit update, flag `0x02` says a trailer follows the payload. A trailer is a sequence of records — an identifier, a length and
two padding bytes — of which this driver acts on one: a **credit report**, four bytes per endpoint (the endpoint, the credits).

| Step | Message | Id | Direction | What is in it |
|---|---|---|---|---|
| 601 | ready | 1 | from the firmware | word 0 the identifier (15:0) and the credit count (31:16); word 1 the credit size (15:0) and how many endpoints it has (23:16); the extended form adds its version (7:0) and a bundle limit (15:8). Nothing may be sent before it, and a count or size of zero ends the level |
| 602 | connect a service | 2 | request | word 0 the identifier (15:0) and the service (31:16) = `0x0100`, the firmware's message interface; word 1 the connection flags (15:0): how many of its receive buffers the service may have (15:8) and, for everything but a message interface, "no credit flow control" (bit 3) |
| 602 | its answer | 3 | from the firmware | word 0 the identifier and the service; word 1 the status (7:0, 0 success, 1 not found, 2 failed, 3 no resources, 4 no endpoints left), the endpoint it gave (15:8) and the largest message it takes (31:16) |
| 603 | set-up complete | 5 | request | word 0 the identifier; two words of set-up flags and a bundle limit, both zero here (no message bundling) |

**The firmware's message interface (WMI).** A command is a 4-byte header — the command identifier in its low 24 bits — and then
TLVs; an event is the same header and then TLVs. A TLV's header is one word: the value's length (15:0) and the tag (31:16), and
the value is padded to a whole number of words. A structure inside a TLV is its members in order, one word each (a MAC address
is six bytes and two of padding), and a structure that stands on its own begins with its own TLV header. An array of structures
is a TLV of tag `0x12` whose value is those structures, each with its header; an array of words is tag `0x10`, of bytes `0x11`,
and an array of structures with no headers of their own is tag `0x13`.

| Step | Message | Id | Tag | What is in it |
|---|---|---|---|---|
| 604 | service ready | `0x00001` (event) | `0x020` | 32 words: the firmware's build, its interface versions, the radio chains (word 9), the high- and very-high-throughput capabilities (10, 11), the system capability (15), **how much host memory it wants (18)**, the channels it can scan (19), the board identifier (20), the radios (26), the hardware modes (28) and the chain mask (29). A second TLV (`0x10`) carries its service bitmap |
| 605 | the host's initialisation | `0x00001` (command) | `0x04a` + `0x04b` + `0x12` | the fixed structure (7 words: six of interface version, then the number of memory chunks = 0), then the **resource configuration** (tag `0x04b`, 72 words: 4 virtual devices, 16 peers, 3 offload peers and reorder buffers, 2 peer keys, 32 traffic identifiers, an address-table skid limit of 16, the chain masks, four receive timeouts (100, 100, 100, 40), native-Wi-Fi decapsulation, 4 pending scans, 3 beacon-miss and roam devices, 8 roam profiles, a 1024-entry debug log, 2 group-key devices, 0x400 message descriptors, 10 fragment entries, 1 direct-link device with 8 table entries, 2 beacon-offload devices, 0x20 multicast filters, 0x16 wake filters, the two flag words `0x40020` and `0x40200`, "use a radio identifier" and peer-map version 1), then an empty array of memory chunks |
| 606 | ready | `0x00002` (event) | `0x023` | 15 words: six of interface version, **the chip's own MAC address** (words 6-7), the status (8), the traffic-class tables (9), extra addresses (10), peers (11, 12), the address-table index (13) and a log checksum (14) |
| 607 | a virtual device | `0x05001` (command) | `0x056` + `0x12` | 11 words: the identifier (0), the type (1) = 2 a station, the subtype (2) = 0, the MAC address (3-4), how many stream entries follow (5) = 2, the radio (6) = 0, two words of multiple-network flags, and a statistics identifier marked invalid (9, 10). Then an array of two stream entries, each 4 words with its own header (tag `0x19b`): the band (0 is 2.4 GHz, 1 is 5 GHz), the transmit streams, the receive streams |
| 608 | a scan | `0x03001` (command) | `0x04d` + `0x10` + `0x13` + `0x13` + `0x11` | 39 words: the scan identifier (0) = `0xa000`, the requestor (1) = 1, the virtual device (2), the priority (3), which events to send (4), the active and passive dwell times (5, 6) = 50 and 150 ms, the rest times (7, 8) = 50 and 500 ms, the largest scan time (12) = 20 s, the probe delay (13) = 5 ms, **the flags (14)**: bit 0 passive, bit 4 "report each channel"; how many channels (16), addresses (17) = 1 and names (18) = 0; the priority (3) = 0 and the per-band dwell times (35, 36, 37) = 0, 70 and 70 ms, which are the bytes this firmware's own host puts there; then the channel list as words, an empty array of names, one address entry (the broadcast one, as its host always sends), and an empty array of elements to add to a probe |
| 609 | a scan event | `0x03001` (event) | `0x024` | 7 words: the event type (bit 0 started, 1 completed, 2 on its own channel, 3 on a foreign channel, 4 dequeued, 6 start failed), the reason (0 completed, 1 cancelled, 2 preempted, 3 timed out, 4 an internal failure), the channel, the requestor, the scan identifier and the virtual device |
| 609 | a management frame | `0x07001` (event) | `0x02c` + `0x11` | 17 words: the channel (0, a channel number), **the signal-to-noise ratio (1)** — the strength a list shows is it plus the noise floor of -95 dBm, as this firmware's own host works it out — the rate (2), the radio mode (3), **the frame's length (4)**, **the status (5)**: a frame with the check, decryption or key bits set (`0x01`, `0x08`, `0x20`) is thrown away whole, four per-chain strengths (6-9), the flags (10), a strength word of its own (11, signed, logged but not used), a timestamp delta and two timestamp words (12-14), the radio (15) and **the frequency (16)**. Then the frame itself as bytes: a beacon (subtype `0x80`) or a probe answer (`0x50`), whose third address is the network's and whose body is a timestamp, a beacon interval, a capability word and then the elements |
| 610 | stop the scan | `0x03002` (command) | `0x04e` | 5 words: the requestor (0), the scan identifier, the kind (0 = this scan only), the virtual device (0) and the radio — the firmware matches the scan by its identifier and the kind, and zero is what its own host sends in the other two |
| 610 | delete the device | `0x05002` (command) | `0x057` | one word: the identifier |
