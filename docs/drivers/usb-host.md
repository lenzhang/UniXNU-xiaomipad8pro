# USB host / OTG and the Type-C port: PianoUsbHost

> **Status 2026-11-11: design, offline.** Backlog item B7 (`../BACKLOG-hardware.md`). Built, host-tested and linked: level 1, a
> read-only survey (`piano-usbhost=1`, `package/PianoUsbHost`), inert without its boot argument and separate from PianoDwc3 (device
> mode keeps the controller). **Not run on hardware.** The macOS-side verdict (§5): no xHCI class of our kernel collection attaches
> to a platform nub as shipped; Apple's whole USB host stack does link into the native collection with a larger stand-in (measured,
> §5.2); the path is Apple's `AppleUSBXHCI` on a hand-made nub (QcomDwc3's model), first without a subclass. Template: `README.md`.

## 1. Purpose and gate

Ubuntu made the single USB-C port a dual-role port (v0.5.0, v0.6.0, v1.1.0): USB sticks, keyboards, mice and powered docks with
pass-through charging, 5 Gbit/s in both plug orientations, role switching with a computer, USB recovery after a USB-C display
unplug. Natively the port is PianoDwc3's device-mode debug channel (`usb-device-debug.md`). Host mode gates no login milestone; it
is a daily-use feature (backlog tier 3), and B8 (DisplayPort Alt Mode) builds on its Type-C half.

## 2. Hardware facts

### 2.1 Controller, xHCI, role registers

| Item | Value | Source | State |
|---|---|---|---|
| Controller | `usb@a600000` (`qcom,sm8750-dwc3`), `dr_mode = "otg"`, `usb-role-switch`, `mode-switch`, `maximum-speed = "super-speed"`, `dma-coherent`, SMMU SID 0x40, SPI 133 (`dwc_usb3`); clocks/GDSC/resets as `usb-device-debug.md` §2 | DT | verified |
| xHCI window | the core's first 32 KiB, `0xa600000..0xa607fff` (DWC3_XHCI_REGS_END); globals from +0xc100 | LNX core.h; SUN | verified (source) |
| Role | GCTL (+0xc110) bits 13:12 PRTCAPDIR: 1 host, 2 device, 3 OTG; GSTS bits 1:0 current mode | LNX core.h/drd.c | verified (source); value after the UEFI: **needs-boot** (survey) |
| Host switch (Linux `__dwc3_set_mode`) | old role down; GCTL.CORESOFTRESET pulsed 100 ms only for DWC3 or DWC31 < 1.90a; on a PRTCAP change GUSB3PIPECTL.SUSPHY (b17) and GUSB2PHYCFG.SUSPHY (b6) cleared, then PRTCAPDIR = 1; root ports' PORT_POWER cleared; `xhci-hcd` child; PHYs set to host; U3 SUSPHY set again at xHCI start (U2 stays off: `dis_u2_susphy_quirk`); ENBLSLPM (b8) cleared (`dis_enblslpm_quirk`) | LNX core.c, host.c | verified (source) |
| xHCI quirks for every DWC3 host | `write-64-hi-lo-quirk` (a 64-bit register's high half first), `xhci-sg-trb-cache-size-quirk`; from the DT `usb3-lpm-capable`, `usb2-lpm-disable`; IMOD 40 µs default, one interrupter | LNX host.c, xhci-plat.c | verified (source); SUN also found the hi-lo order in Xiaomi's stock kernel |
| Port count | from the xHCI's Supported Protocol capabilities at run time; no source states it (the glue sees one HS port) | LNX | **needs-boot** (survey) |
| QSCRATCH VBUS/session override | `0xa6f8800`: HS_PHY_CTRL +0x10 b20 UTMI_OTG_VBUS_VALID + b28 SW_SESSVLD_SEL, SS_PHY_CTRL +0x30 b24 LANE0_PWR_PRESENT; set for device, cleared for host (dwc3-qcom's role notifier) | LNX dwc3-qcom.c | verified (source) |
| UTMI clock as PIPE clock | GENERAL_CFG +0x08: set b8 (clock off), b0 + b3, clear b8, 100–1000 µs waits; Ubuntu v1.1.0 uses it in DP-only modes (else the next xHCI reset times out); PianoDwc3 uses it for high-speed device mode | LNX (K:68d28640e1d1); PianoDwc3 | verified (source) |

### 2.2 QMP USB3/DP combo PHY (the SuperSpeed lanes and their Type-C mux)

| Item | Value | Source | State |
|---|---|---|---|
| Block | `phy@88e8000`, 16 KiB, `orientation-switch`, `mode-switch`, USB3_PHY_GDSC, clocks aux `0x39064`, com_aux `0x39068`, pipe `0x3906c`, ref = TCSR USB3 clkref | DT, gcc-sm8750 | verified |
| Layout | Linux's sm8750 config, v8 offsets: DP_COM +0x0000, USB3 serdes +0x1000, PCS +0x1e00, PCS_USB +0x2100, DP PHY +0x3c00 | LNX qmp-combo | verified (source) |
| DP_COM | PHY_MODE_CTRL +0 (b0 USB3, b1 DP: 1 USB3 only, 2 DP 4 lanes, 3 USB3 + DP 2 lanes), SW_RESET +4, POWER_DOWN_CTRL +8 (b0 set = powered), SWI_CTRL +0xc, **TYPEC_CTRL +0x10** (b1 software lane select, b0 1 = reverse, b2 invert CC), TYPEC_PWRDN +0x14, RESET_OVRD +0x1c | LNX dp-com-v3.h; SUN (0x088e8010) | verified (source); reset values unknown |
| Orientation in Linux | `typec_switch_set` power-cycles DP_COM and USB on a change and writes MUX \| (reverse ? VAL : 0); mode from the altmode notification (no DP SVID: USB3 only) | LNX | verified (source) |
| PCS ready | PCS_STATUS1 (+0x1e14) b6 PHYSTATUS = not ready | LNX pcs-v8.h | verified (source) |
| Natively | not used: PianoDwc3 runs high speed with the UTMI clock as PIPE clock; the PHY's state after the UEFI is **needs-boot** (SUN saw COM = 0x01 in its device mode) | PianoDwc3; SUN | — |

### 2.3 Type-C state: four witnesses

| Witness | What | Needs | State |
|---|---|---|---|
| PMIC Type-C block (PMIH010x SID 7, charger + 0x600) | +0x06 sink status (b3 default Rp, b2 1.5 A, b1 3.0 A), +0x08 source status (b3 Rd/open = a USB device, b2 Rd/Ra, b0 Ra/Ra audio, b4 debug), +0x0b misc (b0 attached, b1 CC2, b5 VBUS, b6 **we source**), +0x0a vSafe5V/0V, +0x44 port mode (b1 sink only, b2 source only, 0 dual role) | SPMI observer read (PianoPower's) | verified (source: LNX qcom_pmic_typec_port.c offsets = SUN's); no Linux driver binds it on piano (no DT node) |
| gpio61 | `pmic-glink orientation-gpios`, input with pull-down; ucsi_glink reads it on every connector status: high = reverse | TLMM read | verified (DT, LNX) |
| UCSI (owner 0x800b) | READ_BUF 0x11 / WRITE_BUF 0x12 / NOTIFY 0x13; v1 48-byte buffer; init PPM_RESET → SET_NOTIFICATION_ENABLE → GET_CAPABILITY → per connector GET_CONNECTOR_CAPABILITY, GET_ALTERNATE_MODES, GET_CONNECTOR_STATUS; CONNECTOR_STATUS: b19 connected, b20 we source, b31:29 partner type (1 DFP, 2 UFP, 3 cable, 4 cable + UFP, 5 debug, 6 audio), b86 orientation (UCSI ≥ 2.0); UFP → host, DFP → device | the DSP up (PianoPower level 2) | verified (source) |
| Port notifications (owner 0x800c) | opcode 0x16, SVID in bits 31:16; @12 port, @13 orientation (0 normal, 1 reverse), @14 mux (0 none, 1 USB3 only, 2 DP 4 lanes, 3 USB3 + DP, 4 tunnel), @20 DP pin assignment/HPD; enable 0x10 and ack 0x11 as write requests 0x15 | the DSP up; PianoPower already enables and acks PAN (`PianoMca.c`), payload not decoded | verified (source) |

**Rp bit order.** Linux orders the sink-status bits b3 default, b2 1.5 A, b1 3.0 A; PianoPower's `ChargerPmic.h` decodes the
reverse as *inferred*. Its level 1 measurement on the Mac's port read `0x02`, which is **3.0 A** in Linux's order (the usual for a
Mac's USB-C port) and "500 mA" in PianoPower's log. PianoUsbHost uses Linux's order; PianoPower's decode should be corrected by its
owner (charging.md §2, "Rp bit→current inferred").

**Sink-only port.** SUN read the port mode `0x02` (sink only) when its UEFI starts; Ubuntu's DSP firmware drives the port. With the
DSP down a sink-only port never sees an OTG adapter's Rd: **host detection needs the DSP running** (PianoPower level 2 works
natively since 2026-11-11 07:15, charging.md).

### 2.4 DIO4485, VBUS, repeater, hazards

| Item | Value | Source | State |
|---|---|---|---|
| DIO4485 | `dioo,dio4485` @0x42 on `i2c@a8c000` (QUP1 SE3, gpio44/45 with pull-ups), the connector's SBU endpoint; ID reg 0x00 = 0xf6; 0x04 enable (b7 device, b6:5 SBU, b4:3 USB, ...), 0x05 select; USB/safe state 0x04 = 0x98, 0x05 = 0x18; DP+USB 0xf8 (+0x78 select reversed); DP-only (pin C/E) switches USB off | DT; LNX fsa4480.c | verified (source); power-on defaults and whether D+/D- pass through it: unknown (inferred yes, FSA4480 family; device mode works with the firmware's state) |
| VBUS for OTG | supplied by the DSP's charger firmware itself once it has the board configuration (CP_STATE 0x201c = 1 byte, OTG_CFG 0x201a = 16 bytes `0, 1, 0xffffffff, 0xffffffff`, CID_CFG 0x201b = 0x708, once per DSP generation); notification 10 hands the SC8541 to reverse (OTG), 3 boost; **Linux never enables VBUS itself**; 0x200d/0x200e boost/gate status, 0x21015 Type-C mode, 0x21020 OTG plug-in, 0x21022 sink/source mode (encodings not in the source) | LNX xiaomi_mca.c, sc8541; kernel.md §2.11 | verified (source), inferred (who switches VBUS) |
| OTG config natively | **not in PianoPower**: charging.md lists "the OTG config write 0x201a" as later and PianoPower's write allowlist refuses 0x201a (`tests/power_host.cpp`); the stock UEFI's PmicDxe VBUS function is empty (SUN) | repo; SUN | verified |
| eUSB2 repeater | pmih0108 @0xfd00: host mode forces the 19.2 MHz clock (FORCE_EN_5 0xe8 b6, FORCE_VAL_5 0xed b6), device clears them; SUN: stock host tuning 0x51/54/56/57 = 0f/01/03/05 vs 0a/03/03 in device mode | LNX phy-qcom-eusb2-repeater.c; SUN | verified (source); the owner EE of the repeater's APID: needs-boot (survey) |
| **Hazard K:7dffd78b342d** | a QSCRATCH VBUS-override write from the UCSI role switch while the glue clocks were off (s2idle) reset the SoC; Ubuntu defers it while suspended and shares one role lock with suspend and the Type-C clock updates (K:9834284b801f) | kernel.md §2.11 | verified |
| SPI 133 | one line for both roles; our ArmCpu has `armcpu_spi_attach` (one handler per INTID) but no detach and no mask | power-irq.md §2.1 | verified |

## 3. Ubuntu as the specification

Kernel `piano/dev` (`unixnu-research/ubuntu-ref/kernel`, 7f8d6bedde47; v1.1.0 = d2710cd13b33), read as described behaviour:
- **Roles** (`ucsi.c` partner change → `usb_role_switch` of `usb@a600000`, K:b1eb75bcf65f): UFP partner → host, DFP → device,
  otherwise none (the core treats none as device). The glue's role notifier runs synchronously in the UCSI work, before the
  core's mode work on a freezable queue (`drd.c`) — the s2idle race of K:7dffd78b342d.
- **Orientation**: gpio61 at each connector status (`ucsi_glink.c`), the altmode notification's orientation byte
  (`pmic_glink_altmode.c` → `typec_switch_set` on the QMP PHY and the DIO4485).
- **Mode**: the altmode mux byte → QMP PHY_MODE (USB3 only without a DP SVID), DIO4485 switch words, the glue's UTMI-as-PIPE in
  DP-only modes (v1.1.0: USB recovers after a display unplug).
- **VBUS**: only the DSP; xiaomi_mca sends the board configuration on every DSP generation and follows notifications 3/10.
- **Known limitation** (v0.5.0..v1.1.x, not fixed): a plain OTG adapter re-plugged right after an OTG device is removed may not
  be detected; a charger/computer cable once, or a dock, clears it. No kernel comment explains it (inferred: the DSP's Type-C
  state machine after a source detach).
- **Open in Ubuntu**: hub and DisplayPort after resume (device.md #21); judge sleep power with the cable out (HS PHY not in L2).

## 4. UniXNU templates to build on

| Piece | Base | Licence | Delta for piano |
|---|---|---|---|
| Apple's xHCI on a Qualcomm DWC3 | **QcomDwc3** (`unixnu-research/QcomDwc3` e4efe95, SM8550, iPadOS 27 24A446; UniXNU-sheng keeps AppleUSBXHCI ABI fixtures for 24A446 and macOS 27.3 26C5035r, `scripts/usb-xhci-*.json`): `QcomDwc3Xhci : AppleUSBXHCI` with three overrides (always-on desired power; hi-lo 64-bit read/write) on a `QcomDwc3Nub : IOService` (an IODeviceMemory of 0x8000 at the core, name `usb-host0`, `bus-number`, `registerInterrupt` over `armcpu_spi_attach`, one line routed to the active role's nub); role switch = core soft reset 100 ms + PRTCAPDIR + SUSPHY bits + repeater host flag; host mode "runs and completes commands, enumeration not verified" | BSD-2-Clause-Patent (its two PHY headers are GPL: not used) | our KC is the vma2 27.0.1 (26A434) collection: neither fixture applies (§5.3); our ArmCpu has no `armcpu_spi_mask` |
| UCSI client | QcomDwc3 `QcomUcsi.h` (message layout and command order after Linux, no I/O) | BSD-2-Clause-Patent | on PianoPower's GLINK channel (owner 0x800b next to 0x800a/0x800c) |
| DSP, GLINK, PAN | PianoPower level 2 (DSP start, MCA, PAN enable/ack) | this repo | add UCSI, decode the PAN payload, the OTG board configuration (opt-in write) |
| Controller bring-up, SMMU, coherency | PianoDwc3 (takeover order, SMMU stream 0x40, UTMI-as-PIPE) and QcomDwc3's coherence note: Apple's USB code does no cache maintenance, so stream 0x40 gets its own stage-1 context with an identity map of DRAM in write-back inner-shareable 1 GiB blocks (a bypass bank leaves the master non-coherent) | this repo; BSD | PianoDwc3's bypass bank is fine for its own WC rings, not for Apple's xHCI |
| Host-mode facts measured on piano | Project-SunUEFI `PianoUsbHost.c` / host plan (stock `Usb30InitHost` values + GCTL.DSBLCLKGTNG + GUCTL.HSTINAUTORETRY, the No-Op command acceptance test, hi-lo writes, PMIC Rd detection twice 250 ms apart) | BSD-2-Clause-Patent | compiled there, not verified on the tablet |

## 5. IOKit family / ABI: the macOS side

### 5.1 What our kernel collection has (the vma2 26A434 KC's `__PRELINK_INFO`: names and matching dictionaries only)

| Kext | Classes / personalities | Matches | On piano |
|---|---|---|---|
| `com.apple.driver.usb.AppleSynopsysUSBXHCI` | `AppleT8027/8030/8101/8130/8140/8150/8160USBXHCI` | `IONameMatch usb-drd,t81xx` on `AppleARMIODevice`, category `usb-host`; libraries AppleARMPlatform, AppleEmbeddedUSBHost, **AppleTypeCPhy**, IOAccessoryManager, IOPortFamily | no: Apple's own DWC3 SoC subclasses, built around Apple's Type-C PHY, DART and port controller; our ADT has no `usb-drd` node and the real AppleARMPlatform cannot be kept (the source-built kernel covers it) |
| `com.apple.driver.usb.AppleUSBXHCIPCI` | `AppleUSBXHCIPCI` (+ AMD, ASMedia, Fresco, Intel AR/TR variants) | `IOPCIClassMatch 0x0c033000` on `IOPCIDevice` | only on an IOPCIDevice nub (§5.3 B) |
| `com.apple.driver.usb.AppleEmbeddedUSBXHCIPCI` | ASM3142, FL1100 | `IONameMatch usb-xhci,…` on `IOPCIDevice` | as above, vendor-specific |
| `com.apple.driver.usb.AppleUSBXHCI` | `AppleUSBXHCI` (base, no personality) | — | a hand-made nub + a personality or subclass (§5.3 A) |
| `com.apple.iokit.IOUSBHostFamily` 1.2, AppleUSBHub, AppleUSBHostCompositeDevice, IOUSBHostHIDDevice, IOUSBMassStorageDriver, usb.cdc/ecm/ncm, ethernet.asix, realtek8153patcher, AppleUSBAudio | the client stack | on `IOUSBHostDevice`/`Interface` | yes, once a controller runs |
| `AppleUSBUserHCI`, `AppleUSBVHCI` | user-space / virtual host controllers | `AppleUSBUserHCIUserClient` | private user-client protocol: no |
| `AppleVirtualPlatformPCIE` (AppleVirtualPlatform) | the VM's ECAM host bridge (`pcie,vmapple1`) | AppleARMIODevice | no: ECAM is memory-mapped config space; we cannot emulate BAR sizing in RAM |

### 5.2 Measured: Apple's host stack links into the native collection

`kc-build27-piano.py` (the native KC's builder) with our native set (ARMPlatformStandIn, NativeUfs, NativeRamBlock,
NativeGopDisplay, PianoDwc3, PianoNullEthernet) plus `--keep` AppleUSBXHCIPCI, AppleUSBHub, AppleUSBHostCompositeDevice,
IOUSBHostHIDDevice, IOUSBMassStorageDriver, usb.cdc.ncm, usb.cdc.ecm, usb.ethernet.asix (2026-11-11, link only, written to a
scratch path, not booted):
1. As is: refused, `_os_log_coprocessor_with_handle` is not in our kernel: the dependency closure (26 kexts) pulls Apple SoC
   kexts through IOUSBHostFamily → AppleSMC → RTBuddy/AppleA7IOP, and IOAccessoryManager → AppleOnboardSerial.
2. Standing in also for AppleSMC, RTBuddy, AppleA7IOP, AppleARMPMU, AppleEmbeddedTempSensor, AppleOnboardSerial and
   IOSlowAdaptiveClockingFamily: refused with `stand-ins lack:` exactly 13 symbols, `AppleSMCFamily::metaClass`,
   `AppleOnboardSerialSync::metaClass` and 11 `gAppleARMUSBCable*` keys (DetectResourceKey, TypeResourceKey, TypeDetached,
   TypeUSBHost/USB3Host(+AltConfig), TypeUSBDevice/USB3Device(+NeedsAuthentication)).
3. With those defined in a scratch stand-in: **linked**, 64 kexts (+17: IOUSBHostFamily, AppleUSBXHCI, AppleUSBXHCIPCI,
   AppleUSBCommon, AppleUSBHostMergeProperties, IOAccessoryManager, IOPortFamily, the hub, composite, HID, mass storage + SCSI
   family, CDC/NCM/ECM/networking, ASIX), stand-ins for 10 kexts with **31 pointers and 0 branches** rebound (only metaclass and
   key references, no call into a stood-in kext), every vtable check 0 problem slots.

UniXNU-sheng's iPadOS board leaves AppleOnboardSerial and AppleSMC out for the same reason ("only through a metaclass in an
OSDynamicCast"). What the cable keys must hold (OSSymbols; QcomDwc3 publishes `AppleUSBCableType = USBHost` on the resource
service in host mode) is to be settled before H6, not by reading Apple's code.

### 5.3 Options and effort

| | Option | Private ABI | Effort | Main risks |
|---|---|---|---|---|
| **A1** | `AppleUSBXHCI` on our nub through a **codeless personality** (IOClass AppleUSBXHCI, IOProviderClass `PianoUsbHostNub`), QcomDwc3's nub (memory, name, bus-number, interrupts) | none (IOService public; the stand-in symbols) | ~1 week after H5 | the class may be abstract; the base class's 64-bit write order (DWC3 needs hi-lo); the controller idling to off with no Type-C transport to wake it |
| **A2** | QcomDwc3's subclass (3 overrides) | AppleUSBXHCI's vtable for 26A434 (slot indices + PAC diversities; UniXNU-sheng's fixtures are for 24A446 and 26C5035r) | +1 week over A1 | deriving the fixture is binary analysis beyond this task's rule: **owner decision** |
| **B** | a fake PCI host bridge: our `IOPCIBridge` subclass with a software config space (class 0x0c0330, BAR0 = 0xa600000/32 KiB, no MSI capability), an `IOPCIDevice` nub, our IOInterruptController over `armcpu_spi_attach`; Apple's `AppleUSBXHCIPCI` unchanged | IOPCIBridge/IOPCIDevice public SDK headers; IOPCIFamily is open source (APSL) to follow its behaviour | 3–6 weeks | IOPCIFamily's configurator, power and link management expecting a real host bridge; the same hi-lo order question |
| **C** | our own minimal host stack without IOUSBHostFamily: xHCI + hub + HID boot protocol + bulk-only mass storage + CDC-NCM/ECM host, publishing IOHIDDevice / IOBlockStorageDevice / IOEthernetController (all proven here) | none | 6–10 weeks for that set | narrow device coverage (no USB audio, no vendor class drivers) |
| D | user space (`AppleUSBUserHCI`) | private user-client protocol | — | no |

For A and B, a 32-bit IOVA window would defuse the hi-lo hazard and give coherency without a subclass: our own IOMapper on
stream 0x40 (a stage-1 context; IOMapper is our source-built kernel's class), if Apple's xHCI takes its provider's mapper —
**needs-boot** (read DCBAAP/ERSTBA after its start). Without a mapper the identity stage-1 map of §4 gives coherency only.

**Verdict.** No xHCI class in our KC attaches to a platform nub as shipped, but the generic Apple xHCI and its whole client stack
are present and link natively (§5.2). Go A1 → A2 (with the owner's approval of the fixture) → B; C only if Apple's xHCI cannot be
made to run. Effort to "keyboard, mouse and stick on an OTG adapter at high speed": ~3–4 weeks of code plus attended sessions; to
Ubuntu parity (5 Gbit/s both orientations, docks with pass-through charging, role switching with the Mac): ~6–9 weeks (§8).

## 6. Design

### 6.1 Roles: one owner of the controller

- **Policy** (`UsbHostSurvey.h decideRole`, host-tested): UCSI first when the DSP's port manager answers (UFP or cable + UFP →
  host; DFP → device; audio → none); else the PMIC: a sink's Rd while we source → host (OTG adapter, bus-powered device); a source's
  Rp → device (computer, charger, or a dock before UCSI says otherwise); **nothing attached → device**, so the debug cable always
  finds PianoDwc3 armed. A charger without data in device mode is harmless (no host traffic, no override harm).
- **Docks**: the dock sources power (we sink, Rp) and, after its PD data-role swap handled by the DSP's PD stack, is a UFP: only
  UCSI's partner type (or the data role) tells host mode; pass-through charging continues (PianoPower's policy is unchanged).
- **Owner of the core**: one role manager (later levels of PianoUsbHost) holds a role lock that every QSCRATCH or core write, every
  clock gate and the suspend path take (Ubuntu's K:9834284b801f); a write happens only after re-reading the GDSC and the four
  CBCRs as on in the same locked section (K:7dffd78b342d). PianoDwc3 gets a small default-off hand-off (stop RUN_STOP, wait
  DEVCTRLHLT, release the event buffer; later reclaim) at H8; until then host tests run with `piano-usb=0`.

### 6.2 Switch sequences (after QcomDwc3 and Linux)

- **device → host**: PianoDwc3 stopped; VBUS override off (clocks checked); GCTL soft reset 100 ms if the IP needs it (survey:
  GSNPSID/GVERSION); PRTCAPDIR = 1; GUSB2PHYCFG SUSPHY/ENBLSLPM off, GUSB3PIPECTL SUSPHY off; repeater host flag (SPMI write: owner
  check first); UTMI-as-PIPE for high speed only (H5/H6) or the QMP PHY in USB3-only mode (H7); then the xHCI nub.
- **host → device**: the xHCI terminated (HCHalted read), soft reset, PRTCAPDIR = 2, override on, repeater device, PianoDwc3's
  takeover again; its replay of the retained log shows the host session on the Mac.

### 6.3 Orientation and lanes

High speed needs no orientation (both D+/D- pairs meet in the receptacle; the DIO4485 is left in its USB state). SuperSpeed (H7):
the PHY in USB3-only mode with TYPEC_CTRL = software select | (reverse ? 1 : 0); reverse from the PMIC's CC bit, gpio61 and later
UCSI/PAN — the survey's three witnesses must agree on the tablet first. The DIO4485 is touched only for DP (B8).

### 6.4 VBUS

Host for a bus-powered device needs: the DSP up (PianoPower level 2), the port in dual-role mode (the DSP's, 0x21015 to read),
the OTG board configuration (opt-in write `0x201c`, `0x201a`, `0x201b`; PianoPower's allowlist change), then the DSP sources VBUS
by itself (boost status 0x200d, notification 3/10). No native code switches a boost. A powered dock needs none of this.

### 6.5 Debugging while the port is a host

The cable to the Mac is gone in host mode, so: the pstore mailbox (always); a time-limited host session
(`piano-usbhost-back=S`, after QcomDwc3's `usb-back`) that returns to device mode, where PianoDwc3's replay of the retained log
shows what happened; from H6 on, a USB-Ethernet dongle on the dock or the OTG adapter (Apple's CDC-ECM/NCM/ASIX drivers are in
the linked set) for `log stream` and ssh. NCM over device mode (`piano-usb-net=1`) stays the daily debug path.

## 7. Boot args and component

| Boot arg | Default | Effect |
|---|---|---|
| `piano-usbhost=N` | 0 (probe declines, nothing mapped) | 1 read-only survey; 2 survey + watch of the port; higher levels later (§8) |
| `piano-usbhost-delay=S` | 0 | wait S s first |
| `piano-usbhost-xhci=0/1/2` | 1 | 1: capability words when clocked, the rest in host/OTG mode; 2: the rest in device mode too; 0 none |
| `piano-usbhost-qmp=0` | 1 | skip the QMP PHY |
| `piano-usbhost-spmi=0/1` | 1 (0 with `piano-power`) | the PMIC Type-C block and repeater reads (one observer channel serves one reader at a time) |
| `piano-usbhost-watch=S`, `-period=MS` | 300, 1000 | level 2's watch |

Component: `PianoUsbHost` in `configs/k27-native-kexts.txt` (`tooling/k27/kext/PianoUsbHost.json`, bundle
`com.unixnu.driver.PianoUsbHost`, IOService on `IOPlatformExpertDevice`, kpi libraries only); build
`tooling/k27/build-native-piano-usbhost.sh`.

## 8. Hardware test plan and ladder

RAM boots, owner present, Ubuntu returns on reboot. Each step is opt-in by boot argument; H1 writes nothing.

| Step | What | Pass | Effort |
|---|---|---|---|
| H1 survey | `piano-usbhost=1` (+ `piano-usb=2` for the console, or alone) with the Mac cable | the lines below, no hang; facts: PRTCAPDIR, IP version, xHCI caps and ports if clocked, PHY mode/orientation, port mode, repeater owner | built; 1 session |
| H1b watch | `piano-usbhost=2`: plug Mac, charger, OTG adapter with a keyboard, both orientations; again with `piano-power=2 piano-usbhost-spmi=1` | changes logged; DSP off: no attach for the OTG adapter (sink-only); DSP on: source mode + Rd; witnesses agree | 1 session |
| H2 DSP view | PianoPower: read 0x21015/0x21022/0x200d/0x200e/0x21020, decode the PAN payload, UCSI READ_BUF (VERSION, CCI) only | values for each plug case | 1–2 d + 1 session |
| H3 OTG power | opt-in board configuration writes | VBUS on an OTG adapter (keyboard LED), boost status non-zero, no charging regression | 1 d + 1 session |
| H4 UCSI | PPM_RESET … GET_CONNECTOR_STATUS, notifications (from QcomUcsi.h) | partner UFP for the adapter, DFP for the Mac, dock UFP after its swap | 2–3 d + 1 session |
| H5 host core | `piano-usb=0`, our own xHCI probe: role switch, UTMI-as-PIPE, HCRST, rings in a WC region, a No-Op command (completion code 1, the right TRB pointer: SUN's acceptance), PORTSC.CCS with a keyboard, port reset → enabled + speed; back to device | all of it, and device mode works after `piano-usbhost-back` | 3–5 d + 2 sessions |
| H6 Apple stack | the KC keep-list + stand-in extension (§5.2), `PianoUsbHostNub`, coherent stream 0x40, A1 then A2/B | IOUSBHostDevice in ioreg, typed keys, a stick mounts, a USB-Ethernet dongle up | 1–2 weeks + sessions |
| H7 SuperSpeed | QMP USB3-only init (Linux's tables as specification), orientation select | a USB3 stick at 5 Gbit/s (PORTSC speed 4) in both orientations | 1–1.5 weeks |
| H8 live roles | role manager + PianoDwc3 hand-off; docks; the re-plug case | Mac ↔ adapter ↔ dock without reboot, charging through the dock | 3–5 d |
| H9 sleep | role lock with s2idle, no glue write with clocks off | unplug in s2idle without reset (the K:7dffd78b342d case) | 2–4 d |

Expected H1 lines (`grep PianoUsbHost`), Mac cable, DSP down, values to be measured:

```
PianoUsbHost: start: piano-usbhost=1 delay=0 xhci=1 qmp=1 spmi=1 watch=300 period=1000 (DWC3 0xa600000, QMP 0x88e8000, orientation gpio61)
PianoUsbHost: level 1: loads only, nothing written (the SPMI observer's read command is the only store)
PianoUsbHost: spmi: arbiter 0x7…, N APIDs; PMIH010x model 0x56, charger 0x…, Type-C block 0x… (APID n, owner EE 0); eUSB2 repeater APID m
PianoUsbHost: Type-C snk 02 src 00 … misc 2. … mode 02 (sink only) …
PianoUsbHost: Type-C: attached on CC1, we are the sink: a source (Rp: charger, computer or dock; we sink), Rp 3.0 A; VBUS detected
PianoUsbHost: eUSB2 repeater status 80 enable 80 force_en_5 00 force_val_5 00 (device setting), tune [51] 0a [54] 03 [56] .. [57] 03
PianoUsbHost: pin gpio61 orientation (pmic-glink orientation-gpios): cfg … function 0 pull down 2 mA output disabled, in 0
PianoUsbHost: GCC usb30 gdsc … : core powered and clocked          (or NOT powered/clocked: then no DWC3/xHCI line has values)
PianoUsbHost: DWC3 DWC_usb31 GSNPSID 3331…: GCTL … PRTCAPDIR 2 = device, GSTS … (current mode device)
PianoUsbHost: xHCI caps …: an xHCI
PianoUsbHost: xHCI version 1.10, caplength 0x20, … ports, AC64 1 …
PianoUsbHost: xHCI: operational and port registers not read (the core is in device mode (piano-usbhost-xhci=2 walks anyway))
PianoUsbHost: QMP combo PHY: not read (its power domain or clocks are off)     (or the DP_COM/PCS lines)
PianoUsbHost: orientation: PMIC normal, gpio61 normal, PHY -: the witnesses agree
PianoUsbHost: role: the policy would choose device: a source's Rp: … (level 1 changes nothing)
PianoUsbHost: summary: typec cc1 sink rp vbus 1 | gpio61 0 | core on device override | phy off | orientation normal | role device
PianoUsbHost: done: N MMIO loads, 0 MMIO stores; P pages mapped, 0 map failures
```

Fail-safe: a load from a block whose gate read on cannot write anything; a stall is a watchdog reset back to ABL/Ubuntu;
`piano-usbhost-xhci=0`, `-qmp=0`, `-spmi=0` remove each part.

## 9. Fallback

Without `piano-usbhost` nothing changes: PianoDwc3 owns the port in device mode (console, HID, NCM), PianoPower reads the Type-C
state for charging. If host mode fails at any later level, the role manager returns the core to device mode.

## 10. Open questions

1. PRTCAPDIR, the IP revision (soft reset needed?), the root ports and their protocols after the UEFI → H1.
2. Does the PMIC's CC bit agree with gpio61 (high = reverse) and the PHY's select in both orientations? → H1b.
3. Who owns the repeater's and the Type-C block's APIDs for writes (EE 0?) → H1 `spmi:` line.
4. Does the DSP switch the port to dual role and source VBUS by itself after the board configuration, and what do 0x21015 /
   0x21022 / 0x200d read? → H2/H3.
5. Is `AppleUSBXHCI` instantiable through a codeless personality, does its base class write 64-bit registers low first, and does
   it take its provider's IOMapper? → H6 (observe registers from our side).
6. The `gAppleARMUSBCable*` values IOAccessoryManager and IOUSBHostFamily expect → before H6, from behaviour (resource service).
7. One SPI 133 for both roles with no detach/mask in our ArmCpu: a shared dispatcher in PianoDwc3, or `armcpu_spi_mask` in the
   kernel (UniXNU's newer ArmCpu has one) → before H6.
8. The OTG re-plug quirk: reproduce at H8 with the DSP's own state (0x21020) logged.

## 11. Licence note

PianoUsbHost is BSD-2-Clause-Patent (`package/PianoUsbHost/LICENSE`, `UPSTREAM`). Linux (GPL-2.0) is the specification only:
behaviour and offsets cited by file and function, nothing pasted. QcomDwc3 and Project-SunUEFI are BSD-2-Clause-Patent; only facts
and design are taken (their GPL PHY headers are not used). From Apple's kernel collection only kext names, class names, Info.plist
matching dictionaries and the link report's symbol names are used (§5).
