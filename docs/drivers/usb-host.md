# USB host / OTG and the Type-C port: PianoUsbHost

> **Status 2026-11-11: offline, built, host-tested, linked; nothing run on hardware.** Backlog item B7 (`../BACKLOG-hardware.md`).
> `package/PianoUsbHost`, inert without `piano-usbhost`: level 1/2 a read-only survey; **level 5 (H5)** host mode with our own
> minimal xHCI probe (host switch, HCRST, rings, a No-Op command, port connection, optional port reset, way back), host-tested
> against a register model (`XhciHost.h`, §6.2a); **level 6 (H6 preparation, path A1)** Apple's `AppleUSBXHCI` on our
> `PianoUsbHostNub` through a codeless personality, stream 0x40 identity-mapped write-back (`SmmuIdentity.h`, after QcomDwc3),
> interrupt 0 over SPI 133 or polled (§6.5a); **path A2 (2026-11-14, built and host-tested, not booted):**
> `package/PianoXhciHost`, our own three-override subclass of the collection's `AppleUSBXHCI` (slot 292 always-on power, slots 419
> and 421 the 64-bit register pair a DWC3 needs), generated from our ABI data `data/usb-xhci/abi-26C5035r-mac17j.json` with a build
> that fails closed on any collection or vtable drift (§5.4); first 27.3 probe and its checklist: §8.4.
> Apple's host stack links natively with `PIANO_USBHOST_STACK=1` (§5.2a: a new
> stand-in, the cable keys in ARMPlatformStandIn for that build only, the stack's boot-time personalities gated; PianoTouch is left
> out of that build for want of a `__DATA_CONST` gap). The default collection is unchanged but for PianoUsbHost's own objects.
> **Levels 3 and 4 (H3/H4, built and host-tested, 2026-11-11 night):** the port manager on PianoPower's new USB-C relay
> (`package/include/PianoUsbcContract.h`: the DSP's UCSI owner 0x800b for one client kext) and its port events (the port
> notifications 0x800c, read only, `PianoAdspContract.h`): level 4
> runs our UCSI client (`UcsiClient.h`: PPM_RESET, the notification enable, GET_CAPABILITY, GET_CONNECTOR_CAPABILITY,
> GET_CONNECTOR_STATUS, every connector change), decides the role (device with nothing attached, host for a UFP: an OTG adapter, a
> stick, a dock after its swap), reports whether we source VBUS (the DSP's own boost after PianoPower's board configuration) and the
> orientation from three witnesses (the port notification, UCSI 2.x, gpio61), all as properties, event-driven (§6.6). The switch
> between PianoDwc3's device mode and our host mode is **default off** (`piano-usbhost-handoff=5|6` plus PianoDwc3's
> `piano-usb-handoff=1`). Tablet list with expected lines: §8.3.
> Test images and expected lines: §8. Template: `README.md`.

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
| Host switch (Linux `__dwc3_set_mode`) | old role down; GCTL.CORESOFTRESET pulsed 100 ms only for DWC3 or DWC31 < 1.90a (and not towards OTG); on a PRTCAP change GUSB3PIPECTL.SUSPHY (b17) and GUSB2PHYCFG.SUSPHY (b6) cleared, then PRTCAPDIR = 1; root ports' PORT_POWER cleared; `xhci-hcd` child; PHYs set to host; U3 SUSPHY set again at xHCI start (U2 stays off: `dis_u2_susphy_quirk`); ENBLSLPM (b8) cleared (`dis_enblslpm_quirk`) | LNX core.c, host.c | verified (source; re-read in the Ubuntu tree 2026-11-11) |
| Device side down (Linux core init from a non-host role) | DCTL.RUN_STOP 0, DSTS.DEVCTRLHLT (b22); DCTL.CSFTRST (b30) self-clearing, DWC_usb31 ≥ 1.90a / DWC_usb32 polled in 20 ms steps (> 50 ms possible), ≤ 1.80a 50 ms more before the PHY is touched; a DCTL write never carries a link-state change request (bits 8:5) | LNX core.c `dwc3_core_soft_reset`, gadget.c | verified (source) |
| GUCTL.HSTINAUTORETRY (b14) | set by SUN's host session with the stock UsbConfigDxe host values (also GSBUSCFG1 \|= 0x1e00, RXTHRCFG \|= 0x04630000, GUCTL1 \|= 0x600000, GUCTL3 b16, GCTL.DSBLCLKGTNG); mainline dwc3 sets b14 for host/OTG, **the Ubuntu tree read here does not** | SUN `uefi/components/usb-host/PianoUsbHost.c`; LNX | verified (source); level 5 sets b14 only, leaves the rest as found |
| PORTSC write-back | keep only the read-only (CCS, OCA, speed, DR) and read-write-sticky bits (PLS, PP, PIC, WCE/WDE/WOE); PED and the change bits are write-1-to-clear, PR/WPR start resets | LNX xhci-hub.c `xhci_port_state_to_neutral`; XHCI | verified (source) |
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
| UCSI (owner 0x800b) | READ_BUF 0x11 / WRITE_BUF 0x12 / NOTIFY 0x13; v1 48-byte buffer; init PPM_RESET → SET_NOTIFICATION_ENABLE → GET_CAPABILITY → per connector GET_CONNECTOR_CAPABILITY, GET_ALTERNATE_MODES, GET_CONNECTOR_STATUS; CONNECTOR_STATUS: b19 connected, b20 we source, b31:29 partner type (1 DFP, 2 UFP, 3 cable, 4 cable + UFP, 5 debug, 6 audio), b86 orientation (UCSI ≥ 2.0); UFP → host, DFP → device | the DSP up (PianoPower level 2) | verified (source); **served natively** (image G2, 2026-11-11 evening: PianoPower's read of the buffer answered **version 1.10, CCI 0, 48-byte buffer**): so no orientation bit (1.x), the 48-byte write image. Whether the DSP notifies command completions, and what GET_CONNECTOR_STATUS says for each partner: **needs-boot** (§8.3, level 4) |
| Port notifications (owner 0x800c) | opcode 0x16, SVID in bits 31:16; @12 port, @13 orientation (0 normal, 1 reverse), @14 mux (0 none, 1 USB3 only, 2 DP 4 lanes, 3 USB3 + DP, 4 tunnel), @16 VID, @18 SVID, @20 DP pin assignment (5:0)/HPD (6, 7); 32 bytes; enable 0x10 and ack 0x11 as write requests 0x15 | the DSP up; PianoPower enables and acks them (`PianoMca.c`); the relay hands a copy to PianoUsbHost, which decodes it (`Ucsi.h decodePort`) | verified (source); **the enable answered natively** (image G2); the payload per plug: **needs-boot** (§8.3) |

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
| OTG config natively | **in PianoPower since image I** (charging.md §5e): `PianoMca.c board()` sends the stock board configuration on every charger generation before any policy write, at level 2 with writes (`piano-power=2`, `piano-power-board=1` the default; not with `piano-power-write=0`, image G2): 0x201c = 1, 0x201a = the 16-byte constant, 0x201b = 0x708, read back on the `diag board:` line; the allowlist takes these values only. The stock UEFI's PmicDxe VBUS function is empty (SUN) | repo; SUN | verified (source); the DSP sourcing VBUS after it: **needs-boot** (§8.3 H3) |
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
| UCSI client | QcomDwc3 `QcomUcsi.h` (message layout and command order after Linux, no I/O) | BSD-2-Clause-Patent | **done** as `UcsiClient.h` + `Ucsi.h` (written anew from the UCSI specification, the full init, connector changes, the poll fallback, failures and retries; QcomUcsi.h's framing facts), on PianoPower's PMIC_RTR_ADSP_APPS channel through the relay (§6.6) |
| DSP, GLINK, PAN | PianoPower level 2 (DSP start, MCA, PAN enable/ack, the board configuration) | this repo | **done**: the USB-C relay (`PianoUsbcRelay.cpp`, `UsbcRelay.c`: UCSI for one client) and PianoPower's port events (`PortEvents.c`, PianoDp's D2: each port notification, read only), the PAN payload decoded in PianoUsbHost |
| Controller bring-up, SMMU, coherency | PianoDwc3 (takeover order, SMMU stream 0x40, UTMI-as-PIPE) and QcomDwc3's coherence note: Apple's USB code does no cache maintenance, so stream 0x40 gets its own stage-1 context with an identity map of DRAM in write-back inner-shareable 1 GiB blocks (a bypass bank leaves the master non-coherent) | this repo; BSD | PianoDwc3's bypass bank is fine for its own WC rings (and level 5's), not for Apple's xHCI: **done** as `SmmuIdentity.h` (QcomDwc3's `smmuIdentity()` adapted, host-tested), level 6's default |
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
OSDynamicCast"). What the cable keys must hold is to be settled at H6 from behaviour, not by reading Apple's code. Correction to an
earlier reading: QcomDwc3 publishes `AppleUSBCableType = USBHost` on the resource service when it enters the **device** role (a USB
host is on the cable: lockdownd's launch event) and removes it when it leaves that role; in the host role it publishes nothing.

### 5.2a The link with the full native set (`PIANO_USBHOST_STACK=1`, 2026-11-11, linked, not booted)

`PIANO_USBHOST_STACK=1 tooling/k27/build-native-image.sh` adds two descriptors to the native list and nothing else changes:
- `tooling/k27/kext/PianoUsbHostStack.json`, a **codeless** bundle (`kc-build.py`: no objects, a plist entry only): its libraries are
  the stack's roots (AppleUSBXHCI, IOUSBHostFamily, AppleUSBHub, AppleUSBHostCompositeDevice, IOUSBHostHIDDevice,
  IOUSBMassStorageDriver, usb.cdc.ncm, usb.cdc.ecm, usb.ethernet.asix; not AppleUSBXHCIPCI), its one personality is A1's
  (`IOClass AppleUSBXHCI`, `IOProviderClass PianoUsbHostNub`, `IONameMatch usb-host0`, `CFBundleIdentifier
  com.apple.driver.usb.AppleUSBXHCI`), and its `patch_personalities` put the stack's three boot-time matchers on IOResources
  (IOUSBHostFamily's AppleUSBHostResources and AppleUSBHostResourcesTypeC, IOUSBMassStorageDriver's IOUSBMassStorageResource, all
  `IOResourceMatch IOKit` as shipped) behind the resource **PianoUsbHostStack**, which only `piano-usbhost=6` publishes.
- `package/PianoUsbHostStandIn` (`stands_in_for` the seven Apple SoC kexts of §5.2 step 2): the two metaclasses
  `AppleSMCFamily` and `AppleOnboardSerialSync` (IOService subclasses no object belongs to).
- The link report then said the eleven `gAppleARMUSBCable*` keys are **AppleARMPlatform's** (still missing with them defined in the
  new stand-in: `kc-build.py` rebinds a stood-in kext's symbols to that kext's one stand-in). They are in
  `package/ARMPlatformStandIn/UsbCableKeys.cpp`, compiled into ARMPlatformStandIn only when `PIANO_USBHOST_STACK=1`
  (`build-native-standin.sh`); the default build's objects are byte-identical to before (`ARMPlatformStandIn.cpp` untouched). Their
  type (`const OSSymbol *`, as AppleARMPlatform's other keys) and values (`AppleUSBCableType`, `USBHost`, `Detached`, ...) are
  inferred from the names and QcomDwc3's behaviour.

Measured (`out/k27/kc-k27-native.log`): **64 kexts kept** (+16 Apple kexts), the native kexts linked, every vtable check 0 problem
slots, **stand-ins for 10 kexts: 31 pointers and 0 branches rebound**. `tooling/k27/check-usbhost-stack.py` (run by the build) reads
the linked collection's plist: 17 bundles only the stack pulls in, 3 personalities behind the gate, no stack personality on IOResources
without it; the only boot-provider personalities are IOAccessoryManager's name matches (`dock,0pin`, `dock,30pin`, `dock,usb-ufp` on
IOService, `port` on IOPlatformDevice), and piano's ADT has no such node (`tooling/adt/adt-dump.py`). So without `piano-usbhost=6`
none of the stack loads.

**PianoTouch is left out of that build** (`PIANO_USBHOST_STACK_DROP`, default `PianoTouch`; inert without `piano-touch=N`): kept kexts
stay at their collection addresses and native segments are placed first-fit in the gaps; IOUSBHostFamily and IOAccessoryManager take
the 0x8d6f0-byte `__DATA_CONST` gap PianoTouch used, the only gap left that holds PianoTouch's 0x79690 bytes (its calibration tables)
is the one PianoA830's 0x402a0 bytes take first, and PianoTouch must link after PianoA830 (it imports PianoA830's lock word). A
largest-first or best-fit placement in the KC builder (the k27 tree's, read-only here) would fit both.

### 5.3 Options and effort

| | Option | Private ABI | Effort | Main risks |
|---|---|---|---|---|
| **A1** | `AppleUSBXHCI` on our nub through a **codeless personality** (IOClass AppleUSBXHCI, IOProviderClass `PianoUsbHostNub`), QcomDwc3's nub (memory, name, bus-number, interrupts) | none (IOService public; the stand-in symbols) | ~1 week after H5; **prepared** (§5.2a, §6.5a, H6 images §8.2) | the class may be abstract; the base class's 64-bit write order (DWC3 needs hi-lo); the controller idling to off with no Type-C transport to wake it |
| **A2** | our own subclass with 3 overrides (`package/PianoXhciHost`) | AppleUSBXHCI's vtable of the 27.3 mac17j collection (slot numbers + PAC diversities): **our data file** `data/usb-xhci/abi-26C5035r-mac17j.json` | **built and host-tested** 2026-11-14 (§5.4); owner approved the analysis the same day | the slot layout is pinned to one collection (the build gate refuses another); a thunk-bound inherited slot; the same hi-lo, coherency and idle-power questions until a tablet log |
| **B** | a fake PCI host bridge: our `IOPCIBridge` subclass with a software config space (class 0x0c0330, BAR0 = 0xa600000/32 KiB, no MSI capability), an `IOPCIDevice` nub, our IOInterruptController over `armcpu_spi_attach`; Apple's `AppleUSBXHCIPCI` unchanged | IOPCIBridge/IOPCIDevice public SDK headers; IOPCIFamily is open source (APSL) to follow its behaviour | 3–6 weeks | IOPCIFamily's configurator, power and link management expecting a real host bridge; the same hi-lo order question |
| **C** | our own minimal host stack without IOUSBHostFamily: xHCI + hub + HID boot protocol + bulk-only mass storage + CDC-NCM/ECM host, publishing IOHIDDevice / IOBlockStorageDevice / IOEthernetController (all proven here) | none | 6–10 weeks for that set | narrow device coverage (no USB audio, no vendor class drivers) |
| D | user space (`AppleUSBUserHCI`) | private user-client protocol | — | no |

For A and B, a 32-bit IOVA window would defuse the hi-lo hazard and give coherency without a subclass: our own IOMapper on
stream 0x40 (a stage-1 context; IOMapper is our source-built kernel's class), if Apple's xHCI takes its provider's mapper —
**needs-boot** (read DCBAAP/ERSTBA after its start). Without a mapper the identity stage-1 map of §4 gives coherency only.

**Verdict.** No xHCI class in our KC attaches to a platform nub as shipped, but the generic Apple xHCI and its whole client stack
are present and link natively (§5.2). Go A1 → A2 (the fixture's binary analysis was approved 2026-11-14 15:33, §5.3) → B; C only if Apple's xHCI cannot be
made to run. Effort to "keyboard, mouse and stick on an OTG adapter at high speed": ~3–4 weeks of code plus attended sessions; to
Ubuntu parity (5 Gbit/s both orientations, docks with pass-through charging, role switching with the Mac): ~6–9 weeks (§8).
**2026-11-14:** A2 is built (§5.4). It is the better first boot of the two, because A1's two open risks (an abstract class, and a
64-bit write that reaches a 4-byte helper in this collection) are exactly what A2 removes; A1 stays in the tree as the control.

### 5.4 Path A2: `PianoXhciHost`, our subclass of the collection's `AppleUSBXHCI` (built, host-tested, not booted)

The owner approved the binary analysis on 2026-11-14; the offline result is `private/host-checks/20261114-xhci/REPORT.md` (local
only). What it established about the 27.3 mac17j collection (`out/inputs/k273/kc/kc273-mac17j.bin`, sha256 `d1003b51…`): the class
has **427 slots**, instance size **0xf88**, no pure virtual slot; the minimal subclass needs exactly three overrides, and the
reviewed UniXNU-sheng fixture pins **this exact collection**, so no new extraction was needed.

**The data file.** `data/usb-xhci/abi-26C5035r-mac17j.json` (+ `data/usb-xhci/README.md`) is ours: slot numbers, PAC diversities,
the symbol every slot must reach, the instance size, the fabricated declarations of the private slots, and the three slots we
implement. Provenance (the fixture's repository, commit and sha256; the collection's sha256) is in the file, and every value was
re-derived from our own collection with `tooling/k27/xhci-abi.py check-kc`. No Apple code, no Apple bytes; the generated header is
a build output.

**The subclass** (`package/PianoXhciHost`, BSD-2-Clause-Patent, own code): no instance field, three overrides —

| Slot | Override | Why |
|---:|---|---|
| 292 | desired/default power state = 3 (on) | with no Apple Type-C transport, an idle root port would let the controller fall to a state nothing wakes it from; our role manager creates it only while a peripheral is on the cable |
| 419 | 64-bit read: two 32-bit loads, low half first | symmetry with the write; one 8-byte access is not promised by this controller |
| 421 | 64-bit write: high half first, barrier, low half, barrier | Linux's `write-64-hi-lo-quirk` for every DWC3 host. **The base class's 64-bit write reaches a 4-byte helper in this collection**, so a ring above 4 GiB would be latched with half an address; `regOr64` / `regAnd64` dispatch through 419 and 421, so these two fix them too |

Nothing else moves into the class: the window, the interrupt, the role switch, the PHY, VBUS, clocks, the SMMU route and the
halted/reset controller stay in `PianoUsbHost` (§6.5a). `AppleUSBXHCIARM` and the SoC leaves are **not** the base class (they want
`AppleARMIODevice`, Apple's Type-C PHY, mapper and clock sessions, and they leave the 64-bit accessors at the base anyway).

**The gate (the build fails closed on ABI drift)** — `tooling/k27/build-native-piano-xhcihost.sh`, in order:
1. `xhci-abi.py self-test`: synthetic objects prove the gates still refuse a changed diversity, an added virtual method, a stolen
   slot, a missing slot, another signing key, an unsigned pointer, a foreign binding, the original's destructor in our slot, and a
   collection with another sha256 (9 cases).
2. `tests/reg64_host` on the Mac: the hi-lo order, the barriers, addresses above 4 GiB, the round trips.
3. `xhci-abi.py check-kc $KC_BASE`: the collection is the pinned one — sha256, all **427** slot targets (by symbol name), all 427
   PAC diversities, IA + address diversification on every entry, instance size `0xf88`. A new seed therefore stops the A2 build.
4. `xhci-abi.py header`: `OriginalUsbXhciABI.h` generated (opaque storage, 155 private declarations, `static_assert` on the size).
5. the subclass compiled with the kernel's own recipe.
6. `xhci-abi.py thunks` + `clang`: a fabricated declaration makes the compiler emit `AppleUSBXHCI::<name>`, which no collection
   defines (the collection has, for example, `AppleUSBHostController::setProperties` in slot 100). An assembler label cannot fix
   that: clang then signs the slot with the label's hash instead of the declaration's, which is the one discriminator that must not
   change. So each such name becomes a `bti c; b <the collection's symbol>` thunk, and the collection builder resolves the branch
   **by name** — no address is hard-coded anywhere, and a tail branch keeps the arguments, the link register and Apple's caller.
7. `xhci-abi.py check-object`: the compiled vtable, slot for slot. Measured on 2026-11-14: **427 slots, 250 bound to the
   collection by name, 173 through a checked thunk, ours exactly [292, 419, 421]**, every entry IA + address-diversified with the
   collection's diversity. One slot of the kernel itself (16) holds our kernel's reserved entry where 27.3's has
   `OSMetaClassBase::Dispatch(IORPC)`: our kernel's slot to fill, printed rather than refused.

**The collection.** `PIANO_USBHOST_A2=1 tooling/k273/build-k273-image.sh` implies `PIANO_USBHOST_STACK=1` and links
`PianoXhciHost` + `PianoUsbHostStackA2` (the same codeless bundle and gate as A1 **without** A1's `AppleUSBXHCI` personality), so
only one automatic client can ever match the nub; `tooling/k27/check-usbhost-stack.py` now refuses zero or two personalities on
`PianoUsbHostNub`. One difference from A1, printed by that check rather than refused: our subclass names `AppleUSBXHCI` and
`IOUSBHostFamily` as libraries, so those two kexts load **at boot** instead of on demand. They still match nothing — the generic
`AppleUSBXHCI` has no personality of its own, the family's boot personalities stay behind the gate resource
`PianoUsbHostStack`, and our personality waits for `PianoUsbHostNub`, which only `piano-usbhost=6` creates.

**Running it.** `piano-usbhost=6 piano-usbhost-a2=1`: level 6 as in §6.5a, but the class loaded, allocated and attached is
`PianoXhciHost`. `piano-usbhost-attach=3` (new) tries the personality first and, if no client matches within 10 s, allocates the
class by name and calls init/attach/start itself — one boot answers both "did IOKit matching find our personality" and "does
`start()` accept this nub". The A1 path is unchanged (`piano-usbhost-a2=0` on an A1 collection).

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
  DEVCTRLHLT, release the event buffer; later reclaim) at H8; until then host tests run with `piano-usb=0`, or level 5's
  hand-off by time (§7: no PianoDwc3 code).

### 6.2 Switch sequences (after QcomDwc3 and Linux)

- **device → host**: PianoDwc3 stopped; VBUS override off (clocks checked); GCTL soft reset 100 ms if the IP needs it (survey:
  GSNPSID/GVERSION); PRTCAPDIR = 1; GUSB2PHYCFG SUSPHY/ENBLSLPM off, GUSB3PIPECTL SUSPHY off; repeater host flag (SPMI write: owner
  check first); UTMI-as-PIPE for high speed only (H5/H6) or the QMP PHY in USB3-only mode (H7); then the xHCI nub.
- **host → device**: the xHCI terminated (HCHalted read), soft reset, PRTCAPDIR = 2, override on, repeater device, PianoDwc3's
  takeover again; its replay of the retained log shows the host session on the Mac.

### 6.2a What level 5 does (`XhciHost.h`, host-tested by `tests/xhcihost_host.cpp`)

Every store goes through a Hal; nothing is stored unless the USB30 GDSC and its four branch clocks (master, sleep, cfg_noc AXI,
aggre AXI) read on, and **each QSCRATCH store is preceded by that read again** (the K:7dffd78b342d rule; the test drops a clock
mid-switch and the switch stops before the glue store). Order:
1. In `start()`, on the matching thread (PianoDwc3's rule for the stream-match registers): stream 0x40 through a VA64 bypass bank
   (PianoDwc3's `SmmuStream.h`; kept if it already bypasses). Level 6: the identity-mapped bank instead (§6.5a).
2. Device side down: GEVNTSIZ.INTMASK (SPI 133 stays quiet), DCTL.RUN_STOP 0 (no link-state request bits), DSTS.DEVCTRLHLT ≤ 100 ms.
3. Glue: SS_PHY_CTRL b24 and HS_PHY_CTRL b20/b28 (the VBUS/session override) cleared; the UTMI clock as PIPE clock (GENERAL_CFG: b8
   on, b0+b3, b8 off, 100 µs apart), **before** the soft reset so the PIPE domain has a clock.
4. DCTL.CSFTRST, self-clearing within 250 ms, then 50 ms; GCTL.CORESOFTRESET 100 ms by Linux's rule (`-csr=2`; DWC_usb3 or DWC_usb31
   before 1.90a; `-csr=1` always, 0 never).
5. GUSB2PHYCFG SUSPHY/ENBLSLPM off, GUSB3PIPECTL SUSPHY off, GCTL.PRTCAPDIR = 1, GUCTL.HSTINAUTORETRY; GSTS.CURMOD = host ≤ 10 ms.
6. xHCI (no xHCI store before PRTCAPDIR reads host): CNR clear ≤ 1 s; halt (RS 0, HCH ≤ 100 ms) if running; HCRST, 1 ms, HCRST and
   CNR clear ≤ 1 s each.
7. One DMA region, physically contiguous, 64 KiB aligned, below 4 GiB when possible (then the 64-bit halves' order cannot matter),
   cleaned once and used through a write-combined mapping only: DCBAA at +0, command ring (64 TRBs, a link TRB with toggle) at
   +0x1000, event ring segment (256 TRBs) at +0x2000, ERST at +0x3000, scratchpad array at +0x3040, the scratchpad pages from +0x4000
   (as many as HCSPARAMS2 asks, ≤ 256). CONFIG.MaxSlotsEn = min(8, slots) (`-slots`), DCBAAP, CRCR (RCS 1), IMAN (IP cleared, IE 0),
   ERSTSZ 1, ERDP, ERSTBA last; **every 64-bit register high half first**; DCBAAP/ERSTBA/CONFIG read back.
8. Root ports powered when PPC and off (neutral write-back | PP), 20 ms; RS 1 (no INTE), HCH clear ≤ 100 ms, no HSE/HCE.
9. A No-Op command (TRB type 23, control word with the cycle bit stored last, doorbell 0): within 100 ms a Command Completion event
   (type 33) with **code 1 and the No-Op TRB's address** (SUN's acceptance test); ERDP advanced with EHB.
10. Ports: each PORTSC logged; with `-reset=1` a connected USB 2 port reset (PR, PRC within 500 ms) → PED and its speed; change
    bits acknowledged (only those set); later port status change events drained every period.
11. The watch (`-watch`, or `-back` seconds): a line per PORTSC change (connect, disconnect, speed), resets of new USB 2 connections.
12. End: RS 0, HCH, HCRST (the controller forgets the rings; the region stays allocated). `-back=S`: GCTL soft reset by the rule,
    PRTCAPDIR = device, GUSB2PHYCFG/GUSB3PIPECTL/GUCTL and the override as found (the glue store behind its gate); the UTMI-as-PIPE
    selection stays (PianoDwc3 selects it too). Else the core stays in the host role until the next boot.

Not done at level 5 (on purpose): the eUSB2 repeater's host clock force (an SPMI write: owner check first; Linux's workaround for L2
entry, not needed for a connection), the QMP PHY (H7), SUN's other stock host values, GCTL.DSBLCLKGTNG (left as found), any
interrupt, any role lock shared with other kexts (level 5 is the only writer: PianoDwc3 off or time-separated, §7).

### 6.3 Orientation and lanes

High speed needs no orientation (both D+/D- pairs meet in the receptacle; the DIO4485 is left in its USB state). SuperSpeed (H7):
the PHY in USB3-only mode with TYPEC_CTRL = software select | (reverse ? 1 : 0); reverse from the PMIC's CC bit, gpio61 and later
UCSI/PAN — the survey's three witnesses must agree on the tablet first. The DIO4485 is touched only for DP (B8).

### 6.4 VBUS (H3)

Host for a bus-powered device needs the DSP up (PianoPower level 2) with its board configuration (`piano-power=2`, writes on: the
default; PianoMca's `board()` sends 0x201c, 0x201a, 0x201b on every charger generation, §2.4). The DSP's firmware then sources VBUS by
itself when it sees a sink's Rd (an OTG adapter, a bus-powered stick); its Type-C state machine also needs its port notifications
acknowledged (PianoPower does that) and is a dual-role port once it runs (the sink-only `0x02` SUN read was the port before the DSP).
**No native code switches a boost, and none is planned**: no VBUS write exists anywhere (the relay refuses anything but UCSI reads and
writes; UCSI's SET_PDR, the only power-role command, exists in the codec only and nothing sends it). A powered dock needs none of
this (it sources).

What "we source VBUS" looks like, the witnesses to read on the tablet (all read only, none of them new writes):

| Line | Sourcing (an OTG adapter or a stick on the port) | Sinking (the Mac, a charger) | Source |
|---|---|---|---|
| PianoUsbHost `ucsi: connector 1 status …` (level 4) | `connected, partner UFP …, we are the provider (we source VBUS)` | `partner DFP …, we are the consumer (we sink)` | UCSI b20 |
| PianoUsbHost `piano-usbhost-vbus` property | `source` | `sink` | the same |
| PianoPower `[Ns] usb: …` (every report) | `a device is attached (we source)`, misc b6 set, b5 VBUS | `a source is attached (charger present)` | PMIC Type-C block, Linux's offsets |
| PianoPower `[Ns] diag port: …` | OTG boost (0x200d) and gate (0x200e) **non-zero**, plug-in (0x21020) non-zero, sink/source (0x21022) the source value, Type-C mode (0x21015) the source value | 0, 0, 0, the sink value | xiaomi_mca names; **encodings not in the source: the tablet's two readings are the table** |
| PianoPower `[Ns] mca: bus … mV` | about 5000 mV (our own boost) with the bus current our load | the source's | 0x20002 |
| the device | a stick's LED, a keyboard's LEDs at power-up | — | eyes |

A gap to know: PianoPower's built-in policy sees a bus above 4 V and counts it as a source attached; with our own 5 V on the bus it
would write its input limit and charge current for "a source" of unknown type (500 mA in, the 5 V row). Harmless for the boost as far
as the source says (the firmware's reverse mode does not take input), but it is a policy decision for PianoPower's owner (charging.md):
ignore the bus while 0x200d reads boosting, or while UCSI says we are the provider.

### 6.5 Debugging while the port is a host

The cable to the Mac is gone in host mode, so: the pstore mailbox (always); a time-limited host session
(`piano-usbhost-back=S`, after QcomDwc3's `usb-back`) that returns to device mode, where PianoDwc3's replay of the retained log
shows what happened; from H6 on, a USB-Ethernet dongle on the dock or the OTG adapter (Apple's CDC-ECM/NCM/ASIX drivers are in
the linked set) for `log stream` and ssh. NCM over device mode (`piano-usb-net=1`) stays the daily debug path.

### 6.5a Level 6: Apple's xHCI on `PianoUsbHostNub` (path A1, prepared)

- **SMMU** (`-smmu=2`, the default at level 6): stream 0x40 through its own stage-1 context bank, a level-1 table of 1 GiB identity
  blocks from 2 GiB (Normal write-back, inner shareable), so the DWC3's DMA is coherent without cache maintenance (QcomDwc3's
  `smmuIdentity()`, adapted as `SmmuIdentity.h`: an exact entry PianoDwc3 or the firmware left is redirected in place, banks other
  streams use or that translate are skipped, from one below the top; host-tested on piano's SMMU geometry). No IOMapper: Apple's
  DMA addresses are physical, and the identity map turns them back into themselves.
- **Switch and probe**: §6.2a steps 2–9 (`-probe=1`), then RS 0 + HCRST, so Apple's driver gets a halted, reset controller in host
  mode; the No-Op result says whether the hardware path works before Apple's code is involved.
- **Gate**: `publishResource("PianoUsbHostStack")` → IOUSBHostFamily's AppleUSBHostResources(TypeC) and the mass-storage resource
  match and load the stack (≤ 11 s waited); `AppleUSBXHCI`'s kext loaded by identifier and its metaclass looked up (logged).
- **Nub** (QcomDwc3's `QcomDwc3Nub` facts, written anew): `PianoUsbHostNub : IOService`, device memory 0 = `0xa600000 + 0x8000`,
  name `usb-host0`, `bus-number` = 4 zero bytes, attached to the platform expert. Interrupt source 0 (level): `registerInterrupt`
  attaches SPI 133 through `PianoIrq.h`'s `SpiLine` (ArmCpu's `armcpu_spi_attach`: one handler per INTID, no detach, no mask), then
  masks it; `enableInterrupt`/`disableInterrupt` write the distributor's ISENABLER/ICENABLER bit (write-one, no read-modify-write:
  our kernel has no `armcpu_spi_mask`); the handler calls the registered action (IOFilterInterruptEventSource's) in primary context;
  a storm (20000 entries in 100 ms) disables the line for good. `-irq=0`, or an attach that fails (PianoDwc3's `piano-usb-irq=1` owns
  the INTID), polls: a thread calls the action every 1 ms while it is enabled, interrupts off around it.
- **Attach**: `-attach=1` registers the nub and waits ≤ 10 s for a client (the codeless personality of §5.2a); `-attach=2` allocates
  `AppleUSBXHCI` by name and calls init (IOClass/IOProviderClass/CFBundleIdentifier table), attach and start itself (QcomDwc3's
  start of its subclass, without one), logging the allocation and start results; `-attach=0` makes the nub only.
- **Watch** (`-watch`, or `-back`): USBCMD, USBSTS, PORTSC 1/2, DCBAAP, ERSTBA, ERDP **read from our side** (are Apple's
  structures below 4 GiB? the hi-lo question), the client's power state, the interrupt counters, and the IOUSBHostDevice services
  with the last one's idVendor:idProduct; a line per change and every 30 s. `-back=S`: the nub terminated (Apple's family stops its
  devices and the controller), RS 0 + HCRST, back to device mode.

### 6.6 Levels 3 and 4: the port manager (H3/H4, built and host-tested)

**The path to the DSP.** UCSI is not a QRTR service: the DSP serves it as owner 0x800b on the GLINK channel PMIC_RTR_ADSP_APPS,
which PianoPower owns (its charger owner 0x800a and the port owner 0x800c are there too), so no `PianoAdspContract.h` port reaches it.
The smallest extension that does is PianoPower's **USB-C relay** for UCSI, new files only plus five small hooks in `PianoPower.cpp`:
- `package/include/PianoUsbcContract.h`: `piano_usbc_attach(client, ctx)` (one client: `state(ready, generation)` and `ucsi(data)`
  callbacks), `piano_usbc_send(data, len)` (queued, at most 4; **only** a 12-byte UCSI read or a 64/544-byte UCSI write of owner 0x800b
  passes: never the charger's owner, never the port owner), `piano_usbc_detach`, `piano_usbc_status`. The port notifications come
  through PianoPower's port events (`PianoAdspContract.h` `piano_port_events`, `PortEvents.c`, read only, shared with PianoDp's D2).
- `package/PianoPower/UsbcRelay.c` (the portable core, `tests/usbc_host.cpp`) and `PianoUsbcRelay.cpp` (the kernel glue: one spin
  lock, callbacks on PianoPower's thread with it dropped, counted for a detach).
- The hooks: the receive loop offers every message to the relay first (a UCSI message is the client's alone while it is ready, so
  PianoMca's counters do not move; everything else, port notifications included, goes on to PianoMca and the port events); after each pass
  `piano_usbc_relay_step(channel, ready, generation, this)` makes the state call and sends the queue; the wait does not block while a
  send is pending; PianoPower's "we run no UCSI client" note says "not ready yet" when a client is attached.
- **Ready** = the charger generation linked (charger_pd up or the 60 s fallback) **and** PianoPower's quiet down (board configuration
  and port enable through) **and** its own UCSI buffer read of that generation answered or given up. A DSP or charger_pd restart is
  ready 0, then ready 1 with a new generation: the client starts over from PPM_RESET; requests of the old generation are dropped.
  PianoPower's other code (its suspend path included) is untouched.

**The UCSI client** (`UcsiClient.h`, Linux's ucsi.c/ucsi_glink.c read as behaviour, the UCSI 1.2/2.0 specification for the layouts):
- Init: a read (VERSION, CCI; a latched RESET_COMPLETE is cleared with SET_NOTIFICATION_ENABLE 0 first); PPM_RESET with the CCI polled
  every 20 ms until RESET_COMPLETE (re-sent while the CCI shows anything else; notifications are off after a reset); SET_NOTIFICATION_
  ENABLE (command complete, error); GET_CAPABILITY; GET_CONNECTOR_CAPABILITY(1); GET_CONNECTOR_STATUS(1); SET_NOTIFICATION_ENABLE with
  every notification the optional features allow (Linux's rule); a last read for a change that came early. Writes use the 48-byte image
  for 1.x, 528 for 2.x.
- A command: the write, its answer (5 s), the completion: a notification → a read whose CCI has COMMAND_COMPLETE (busy CCIs ignored)
  and carries MESSAGE_IN; then ACK_CC_CI (command complete, plus the connector change for a change's status without error) until
  ACK_COMPLETE. An ERROR is followed by GET_ERROR_STATUS (logged). **Poll fallback**: with no notification, a read 250 ms after the
  write's answer, then every 500 ms, up to 10 s; the `ucsi: ready` line counts completions *notified* and *polled*, which answers
  whether the DSP notifies at all.
- Ready: **nothing is sent and nothing polls**. A notification → a read → the CCI's connector field → GET_CONNECTOR_STATUS with the
  change acknowledged → a status line and the role decision. A PPM that keeps reporting changes (more than 10 in 10 s) is not followed
  until a later notification (no timer). A connector number above the capability's count is logged and ignored.
- Failures (a timeout, a refused read or write, an init command refused): logged with the step, a new attempt after 10 s, three per
  generation, then nothing until the next generation.
- SET_UOR (`piano-usbhost-uor=1` DFP, `=2` UFP, once per generation at the first connected status) is a test switch, off by default:
  with the Mac it would ask the Mac for a data-role swap. SET_PDR exists in the codec only.

**Witnesses and decision** (`PortRole.h`): the role is `decideRole()`'s (§6.1) from UCSI's partner type: UFP or powered cable + UFP →
host, DFP → device, debug → device, audio → none, **nothing attached → device**. Level 3 has no UCSI: it publishes the connection and
orientation from the port notification and gpio61, and its role is always device. Power direction: UCSI b20 (provider = we source
VBUS). Orientation: UCSI's b86 (2.x only: not on this 1.10 PPM), the port notification's orientation byte (when its mux is not "no
connection"), gpio61 read at each status (high = reverse, as ucsi_glink reads it; only with a cable in), the PMIC's CC bit when SPMI
is ours (not with PianoPower running); the first known one is published, any disagreement flagged. Published on the PianoUsbHost
service (`ioreg -rc PianoUsbHost -l | grep piano-usbhost`): `piano-usbhost-ucsi`, `-port`, `-role`, `-role-why`, `-vbus`,
`-orientation`, `-handoff`, `-summary`; a `port:` line on every change of the summary.

**Battery.** The port manager's thread sleeps with no timeout while nothing is in flight (`portWait(seen, 0)`): with nothing
attached, after the init, it never wakes unless the DSP sends something. The only timers are a command's completion and answer
timeouts, the reset's 20 ms polls (at most 10 s per generation), the failure retry (three per generation), the switch's debounce and,
during a host session only, the port watch every `piano-usbhost-period` ms. PianoPower's own 5 s charger cycle is unchanged (not ours).

**The switch** (`RoleSwitch`, default off: `piano-usbhost-handoff=0` publishes "would switch to host" and writes nothing). With
`piano-usbhost-handoff=5` (host with our own probe: §6.2a's switch, the No-Op, the root ports, the port watch; `piano-usbhost-reset=1`
for the USB 2 port reset) or `=6` (Apple's xHCI on the nub, §6.5a; needs `PIANO_USBHOST_STACK=1`): host once the decision has read host
for `piano-usbhost-debounce` ms (1000: a dock's data-role swap settles first), device **at once** when it reads anything else (an
unplug gives the port back to the debug cable). A failed host attempt leaves device mode and is not repeated until the partner goes
away. The SMMU route for stream 0x40 is set in `start()` only when the switch is on (bypass for 5, identity for 6).

**The hand-off with PianoDwc3** (`package/PianoDwc3/UsbHandoff.h`, default off on PianoDwc3's side too: `piano-usb-handoff=1`).
PianoUsbHost finds the registered PianoDwc3 service by class and calls its platform function `piano-usb-handoff` (no link
dependency): **release** → PianoDwc3's USB thread, between passes, stops its device controller (RUN_STOP 0, the halt awaited), masks
its event interrupt (and leaves interrupt mode: SPI 133 may be Apple's xHCI's next), parks; then our host switch. **Reclaim** (after
our way back: the xHCI halted and reset, PRTCAPDIR device, the override as found) → PianoDwc3 re-reads the core's GDSC and branch clocks
and only when they read on runs its whole takeover again; its replay of the retained log then shows the host session on the Mac.
`state` answers starting (asked too early: we ask again in 2 s), device, released, failed, and withdraws a pending ask. With PianoDwc3 owning the controller
(`piano-usb>=2`) and no `piano-usb-handoff=1`, PianoUsbHost refuses the switch at `start()` and PianoDwc3 answers "unsupported" anyway.

**Never a VBUS-override store with the clocks off** (K:7dffd78b342d): our glue stores are XhciHost.h's, each right after its five-clock
gate read on; PianoDwc3's QSCRATCH writes (the override, the UTMI select) go through its sleep guard (`UsbSleep.h`'s `qsWrite`: to the
glue only while awake with its clocks read on just then, else recorded and written after the wake), and its reclaim waits out a sleep
and refuses the takeover when the clocks read off. PianoDwc3 releases only while awake (never in a sleep's kept state); while it is
parked `gRunning` is false, so a system sleep is answered by its `setPowerState` without touching the core. A release PianoDwc3 did not
take within 2 s is withdrawn (`state` withdraws a pending ask), so it can never be parked with nobody to reclaim it. What is **not** there
yet is H9's side for our host session: PianoUsbHost is not in the power tree, so a sleep entered during a host session does not stop
our probe or Apple's xHCI first (the switch is default off; do not combine it with sleep tests).

## 7. Boot args and component

| Boot arg | Default | Effect |
|---|---|---|
| `piano-usbhost=N` | 0 (probe declines, nothing mapped) | 1 read-only survey; 2 survey + watch of the port; **3** the survey, then the port manager with the port notifications only; **4** the same with our UCSI client (§6.6; both need `piano-power=2`); **5** the survey, then host mode with our own xHCI probe (§6.2a); **6** the survey, the switch, then Apple's xHCI on our nub (§6.5a); others run as 2 |
| `piano-usbhost-delay=S` | 0 | wait S s first |
| `piano-usbhost-xhci=0/1/2` | 1 | 1: capability words when clocked, the rest in host/OTG mode; 2: the rest in device mode too; 0 none |
| `piano-usbhost-qmp=0` | 1 | skip the QMP PHY |
| `piano-usbhost-spmi=0/1` | 1 (0 with `piano-power`) | the PMIC Type-C block and repeater reads (one observer channel serves one reader at a time) |
| `piano-usbhost-watch=S`, `-period=MS` | 300, 1000 | level 2's watch; levels 5/6: the host session's length when `-back` is 0, and the watch period |
| `piano-usbhost-back=S` | 0 | levels 5/6: the host session lasts S s, then back to device mode; 0 stays in the host role until the next boot |
| `piano-usbhost-csr=0/1/2` | 2 | GCTL.CORESOFTRESET on the role change: never, always, Linux's rule |
| `piano-usbhost-utmi=0/1` | 1 | the UTMI clock as PIPE clock (high speed only) |
| `piano-usbhost-reset=0/1` | 0 | level 5: reset a connected USB 2 port (enabled + speed); 0 connection only |
| `piano-usbhost-smmu=0/1/2` | 1 at level 5, 2 at 6 | stream 0x40: untouched, bypass bank, identity-mapped write-back bank; a failure refuses host mode |
| `piano-usbhost-slots=N` | 8 | MaxSlotsEn of our probe |
| `piano-usbhost-attach=0/1/2/3` | 1 | level 6: the nub only, the personality, an allocation by name, **3** the personality and then the allocation if nothing matched within 10 s (one boot answers both) |
| `piano-usbhost-a2=0/1` | 0 | level 6: 0 Apple's `AppleUSBXHCI` itself (path A1); 1 our `PianoXhciHost` subclass (path A2, §5.4), which needs a `PIANO_USBHOST_A2=1` collection |
| `piano-usbhost-irq=0/1` | 1 | level 6: polled every 1 ms, SPI 133 (polled anyway when the INTID is taken) |
| `piano-usbhost-probe=0/1` | 1 | level 6: our No-Op probe before Apple's driver (and in `-handoff=6`'s host sessions) |
| `piano-usbhost-handoff=0/5/6` | 0 | levels 3/4: **the role switch**: 0 the decision published only ("would switch"); 5 host sessions with our probe; 6 with Apple's xHCI on the nub (`PIANO_USBHOST_STACK=1`). With `piano-usb>=2` it also needs PianoDwc3's `piano-usb-handoff=1`, else refused at start |
| `piano-usbhost-debounce=MS` | 1000 | levels 3/4: how long the decision must read host before the switch (at most 10000); device is at once |
| `piano-usbhost-uor=0/1/2` | 0 | level 4, a test switch: SET_UOR once per generation at the first connected status: 1 ask for DFP (we host), 2 for UFP |
| `piano-usb-handoff=0/1` (PianoDwc3) | 0 | PianoDwc3 answers PianoUsbHost's release/reclaim (UsbHandoff.h) and registers its service for it; 0: the controller is PianoDwc3's for the whole boot |

Levels 5 and 6 refuse (and run the level 1 survey) while PianoDwc3 owns the controller (`piano-usb>=2`). The one exception is level
5's **hand-off by time**: `piano-usbhost-back=S` and `piano-usb-delay >= piano-usbhost-delay + S + 30`: the host session ends, the core
is back in device mode, and PianoDwc3's takeover (its own halt, soft reset and device programming) starts later; its replay of the
retained log then shows the host session on the Mac. No PianoDwc3 code is involved. Levels 3/4 with `piano-usbhost-handoff=5|6` are
the real hand-off (§6.6): PianoDwc3 released and reclaimed through its platform function, both sides default off.

Component: `PianoUsbHost` in `configs/k27-native-kexts.txt` (`tooling/k27/kext/PianoUsbHost.json`, bundle
`com.unixnu.driver.PianoUsbHost`, IOService on `IOPlatformExpertDevice`, kpi libraries and `com.unixnu.driver.PianoPower` for the
relay's `piano_usbc_*` and the port events' `piano_port_*` functions, listed after PianoPower); build
`tooling/k27/build-native-piano-usbhost.sh` (four host tests,
`ucsi_host` the simulated PPM). The relay is PianoPower's (`build-native-piano-power.sh`, `usbc_host`); the hand-off decision is
PianoDwc3's (`tests/usb_handoff_host.cpp`). Level 6 also needs the collection from `PIANO_USBHOST_STACK=1`
(`package/PianoUsbHostStandIn`, `tooling/k27/kext/PianoUsbHostStack.json`, §5.2a); on the default collection it logs the class as
not registered and attaches nothing.

Path A2 adds one component of its own: `PianoXhciHost` (`package/PianoXhciHost`, descriptor
`tooling/k27/kext/PianoXhciHost.json`, bundle `com.unixnu.driver.PianoXhciHost`, library `com.apple.driver.usb.AppleUSBXHCI`
only — see the descriptor's comment: naming `IOUSBHostFamily` as well would make an inlined local symbol ambiguous), build
`tooling/k27/build-native-piano-xhcihost.sh` (one host test, four ABI gates), ABI data `data/usb-xhci/abi-26C5035r-mac17j.json`,
tool `tooling/k27/xhci-abi.py`. It is **not** in `configs/k27-native-kexts.txt`: only `PIANO_USBHOST_A2=1` links it, together with
`PianoUsbHostStackA2` instead of `PianoUsbHostStack` (§5.4).

## 8. Hardware test plan and ladder

RAM boots, owner present, Ubuntu returns on reboot. Each step is opt-in by boot argument; H1 writes nothing.

| Step | What | Pass | Effort |
|---|---|---|---|
| H1 survey | `piano-usbhost=1` (+ `piano-usb=2` for the console, or alone) with the Mac cable | the lines below, no hang; facts: PRTCAPDIR, IP version, xHCI caps and ports if clocked, PHY mode/orientation, port mode, repeater owner | built; 1 session |
| H1b watch | `piano-usbhost=2`: plug Mac, charger, OTG adapter with a keyboard, both orientations; again with `piano-power=2 piano-usbhost-spmi=1` | changes logged; DSP off: no attach for the OTG adapter (sink-only); DSP on: source mode + Rd; witnesses agree | 1 session |
| H2 DSP view | PianoPower: read 0x21015/0x21022/0x200d/0x200e/0x21020, decode the PAN payload, UCSI READ_BUF (VERSION, CCI) only | values for each plug case | 1–2 d + 1 session |
| H3 OTG power | PianoPower's board configuration (level 2 with writes, the default); the witnesses of §6.4; the PianoDwc3 ↔ PianoUsbHost hand-off (default off) | VBUS on an OTG adapter (the stick's LED, `we are the provider`), boost status non-zero, no charging regression | **built and host-tested** (§6.4, §6.6; images A1/A2 §8.3); 1 attended session |
| H4 UCSI | `piano-usbhost=4`: the UCSI client over PianoPower's USB-C relay (PPM_RESET … GET_CONNECTOR_STATUS, every change), the decision and the witnesses as properties | partner UFP for the adapter, DFP for the Mac, dock UFP after its swap; the orientation witnesses agree | **built and host-tested** against a simulated PPM (`tests/ucsi_host.cpp`); U0/U1 unattended, A1 attended (§8.3) |
| H5 host core | `piano-usb=0`, our own xHCI probe: role switch, UTMI-as-PIPE, HCRST, rings in a WC region, a No-Op command (completion code 1, the right TRB pointer: SUN's acceptance), PORTSC.CCS with a keyboard, port reset → enabled + speed; back to device | all of it, and device mode works after `piano-usbhost-back` | **built and host-tested** (`piano-usbhost=5`, §6.2a); 2 sessions (H5a/H5h below; H5c needs VBUS: H3 first) |
| H6 Apple stack | the KC keep-list + stand-in extension (§5.2), `PianoUsbHostNub`, coherent stream 0x40, A1 then A2/B | IOUSBHostDevice in ioreg, typed keys, a stick mounts, a USB-Ethernet dongle up | **A1 prepared** (`piano-usbhost=6`, `PIANO_USBHOST_STACK=1`, §5.2a, §6.5a): links, gated; 1–2 sessions to learn whether A1 runs, then 1–2 weeks |
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

### 8.1 H5 test images (`piano-usbhost=5`)

RAM boots through the native loop (`tooling/native/native-loop.sh IMAGE`: the test runs until `piano-reset-after`, then the dump
image pulls its log from the pstore mailbox; `grep PianoUsbHost logs/native-runs/<ts>-loop/xnu-console.txt`). Built and linked
2026-11-11 (default collection: 48 kexts kept, as before), not booted.

| Image | Build | What it shows |
|---|---|---|
| H5a `out/ram-usbhost5` | `PIANO_RESET_AFTER=300 PIANO_EXTRA_ARGS="piano-usb=0 piano-usbhost=5 piano-usbhost-delay=20 piano-usbhost-watch=180" tooling/k27/build-native-image.sh && rm -rf out/ram-usbhost5 && cp -R out/ram out/ram-usbhost5` | nothing or the Mac cable on the port: the switch, HCRST, the rings, **No-Op PASS**, the ports' state; the core stays in host mode until the reset |
| H5h `out/ram-usbhost5h` | as H5a with `PIANO_EXTRA_ARGS="piano-usb=2 piano-usb-delay=200 piano-usbhost=5 piano-usbhost-delay=20 piano-usbhost-back=120"`, `cp -R out/ram out/ram-usbhost5h`; `python3 tooling/native/usb-console.py --capture 1000 &` | the Mac cable in: the same host session, then **back to device mode** and PianoDwc3's takeover at 200 s: the console enumerates on the Mac and its replay shows the host session's lines (the H5 pass's "device mode works after back") |
| H5c `out/ram-usbhost5r` | as H5a with `piano-usbhost-reset=1` | a USB 2 keyboard on an OTG adapter: `connected 1`, the reset, `enabled 1`, its speed. **Needs VBUS** on the port (the device pulls D+ up only with VBUS): after H3 (the DSP's OTG board configuration), or a self-powered device that does not wait for VBUS |

Expected H5a lines (values to be measured; `…` elides the survey's lines, §8):

```
PianoUsbHost: start: piano-usbhost=5 delay=20 xhci=1 qmp=1 spmi=1 watch=180 period=1000 (DWC3 0xa600000, QMP 0x88e8000, orientation gpio61)
PianoUsbHost: level 5: host mode with our own xHCI probe (writes the core, its glue and the xHCI; polled); back=0 csr=2 utmi=1 reset=0 smmu=1 slots=8
PianoUsbHost: stage 9: SMMU: stream 0x40 through a bypass bank
PianoUsbHost: smmu: stream 0x40: through a bypass bank (non-coherent: our rings are non-cacheable) (groups G banks 83; slot S bank 81; smr 80000040 s2cr 00000051)
                                 (or "kept: it already bypasses")
…
PianoUsbHost: role: the policy would choose device: … (levels 5 and 6 take the host role whatever the cable says)
PianoUsbHost: stage 10: host switch: the device controller stopped, the glue (override off, UTMI as PIPE), soft reset, PRTCAPDIR host
PianoUsbHost: host switch: DWC_usb31 GSNPSID 3331… GVERSION …; found GCTL …2… (PRTCAPDIR 2) DCTL 8… (run/stop 1, halted 1 in N us) GEVNTSIZ …, override 3 of 3, UTMI as PIPE 0|1; DCTL.CSFTRST cleared in N us, GCTL soft reset not needed
PianoUsbHost: host switch: now GCTL …1… (PRTCAPDIR 1) GSTS …1 (CURMOD 1 after N us) GUSB2PHYCFG … GUSB3PIPECTL … GUCTL …; QSCRATCH general … hs … ss …; N stores (5 QSCRATCH, 2 if the UTMI clock was already the PIPE clock; each after the clocks read on): host mode
PianoUsbHost: stage 11: xHCI capabilities
PianoUsbHost: xHCI version 1.10, caplength 0x20, S slots (8 used), P ports, N scratchpad buffers, ERST max M, AC64 1 CSZ … PPC …; doorbells 0x… runtime 0x…; protocols 2; region 0x… bytes
PianoUsbHost: stage 12: DMA region
PianoUsbHost: dma: 0x… bytes at 0x…0000 (below 4 GiB: the 64-bit halves' order cannot matter), write-combined CPU mapping 0x…
PianoUsbHost: stage 13: xHCI halt and HCRST
PianoUsbHost: xHCI reset: USBCMD 00000000 USBSTS 00000001 (was running 0; CNR 0 us, halt 0 us, HCRST + CNR N us): ok
PianoUsbHost: stage 14: xHCI rings: DCBAA, command ring, event ring, ERST, scratchpads
PianoUsbHost: xHCI rings: DCBAAP 0x…0000 CRCR 0x0 ERSTBA 0x…3000 ERDP 0x…2000 CONFIG 00000008 PAGESIZE 00000001 USBSTS 00000001: read back
PianoUsbHost: stage 15: root port power
PianoUsbHost: ports: P, N switched on (PPC …)
PianoUsbHost: stage 16: run (RS 1, polled: no interrupt enabled)
PianoUsbHost: xHCI run: USBCMD 00000001 USBSTS 000000…0 after N us: running
PianoUsbHost: stage 17: No-Op command
PianoUsbHost: No-Op: event …1000 0000000… 01000000 00008401: type 33 code 1 for TRB 0x…1000 (expected 0x…1000) after N us, K other events (ports 0x…), USBSTS …: PASS
PianoUsbHost: stage 18: root ports: connection (and, with piano-usbhost-reset=1, a USB 2 port reset)
PianoUsbHost: port 1 (USB 2) PORTSC 000002a0: connected 0 enabled 0 power 1 reset 0 over-current 0, link state 5, speed -, changes 000
PianoUsbHost: port 2 (USB 3) PORTSC 000002a0: connected 0 enabled 0 power 1 reset 0 over-current 0, link state 5, speed -, changes 000
PianoUsbHost: stage 19: port watch (plug and unplug now)
PianoUsbHost: port watch done: N changes in 180 s; USBSTS …
PianoUsbHost: xHCI stop: halted and reset: USBSTS 00000001: ok
PianoUsbHost: host mode kept (piano-usbhost-back=0): the core stays in the host role until the next boot
PianoUsbHost: host summary: switch ok | No-Op PASS | ports 1:- 2:- | smmu bypass | dma <4G | host kept
PianoUsbHost: stage 7: done
PianoUsbHost: done: N MMIO loads, M MMIO stores; P pages mapped, 0 map failures
```

H5h adds `hand-off by time: host mode for 120 s from 20 s, back to device mode, PianoDwc3's takeover at 200 s (piano-usb-delay)`
after `start:`, `stage 20: back to device mode (piano-usbhost-back)` and `back: GCTL …2… (PRTCAPDIR 2) … override restored 1; N
stores: device role (PianoDwc3's takeover programs the rest)` before the summary (`| back to device`), then PianoDwc3's usual
takeover lines (`usb-device-debug.md`). H5c adds, per connected USB 2 port, `port 1: reset: PORTSC … -> … in N ms: enabled,
high speed` (`full`/`low` for those devices) and `+T s:` lines in the watch.

Reading the failures: `FAILED at stage 10` with `the core is not powered/clocked: nothing written` (the gate reads the GDSC and
four branches, the sleep clock included, which PianoDwc3's own gate does not: the survey's `GCC usb30` line shows which is off;
level 5 switches no clock on), `DCTL.CSFTRST did not clear` (no PIPE clock: `-utmi` was 0 or the selection did not
take), `GSTS.CURMOD does not read host` (the role did not change: try `-csr=1`); stage 13 `CNR stays set` / `HCRST does not clear`
(the host block is not clocked or not out of reset; Ubuntu v1.1.0 saw the same without UTMI-as-PIPE in DP-only modes); stage 16
`HSE` (a DMA the bus refused: the SMMU route; `-smmu=0` to compare); stage 17 `no command completion` (the event ring's address or
the doorbell: compare ERSTBA/ERDP read back), `the completion points at another TRB` (a 64-bit address mis-latched: the region
should be below 4 GiB), a code other than 1 (a TRB the controller rejects).

### 8.2 H6 test images (`piano-usbhost=6`, path A1)

| Image | Build | What it shows |
|---|---|---|
| H6a `out/ram-usbhost6` | `PIANO_USBHOST_STACK=1 PIANO_RESET_AFTER=420 PIANO_EXTRA_ARGS="piano-usb=0 piano-usbhost=6 piano-usbhost-delay=30 piano-usbhost-watch=300" tooling/k27/build-native-image.sh && rm -rf out/ram-usbhost6 && cp -R out/ram out/ram-usbhost6` | the probe PASS, the stack loading through the gate, the nub, whether `AppleUSBXHCI` attaches through the codeless personality and what it does to the controller |
| H6b `out/ram-usbhost6d` | as H6a with `piano-usbhost-attach=2` | the class allocated by name: an abstract class, or `start` refusing our provider, shows here (the A2 decision point) |
| H6c `out/ram-usbhost6p` | as H6a with `piano-usbhost-irq=0` | the same with the interrupt polled (separates an interrupt problem from the rest) |
| H6d `out/ram-usbhost6b` | as H6a with `piano-usbhost-smmu=1` | Apple's xHCI on the non-coherent bypass bank (expected to fail after the first commands: the coherency evidence) |

Expected H6a lines after the survey and the switch (as H5a's, with `smmu=2` and `level 6: attach=1 irq=1 probe=1`):

```
PianoUsbHost: smmu: stream 0x40: identity map of DRAM from 2 GiB, write-back inner shareable (coherent) (groups G banks 83; slot S bank 81; table 0x…; sctlr 00001027 fsr 00000000)
…  (stages 10..17 as H5a: No-Op PASS)
PianoUsbHost: xHCI stop before Apple's driver: halted and reset: USBSTS 00000001: ok
PianoUsbHost: stage 30: the host stack's gate resource PianoUsbHostStack (IOUSBHostFamily's and IOUSBMassStorageDriver's resource personalities)
PianoUsbHost: stack: AppleUSBHostResources up (IOUSBHostFamily loaded through the gate)        (or AppleUSBHostResourcesTypeC)
PianoUsbHost: stack: resource AppleUSBCableType (none)
PianoUsbHost: AppleUSBXHCI: kext load 0, class registered (instance size N)
PianoUsbHost: stage 31: PianoUsbHostNub: usb-host0, the core's first 32 KiB, bus-number 0, interrupt source 0
PianoUsbHost: nub: usb-host0 attached to the platform expert (memory 0xa600000 + 0x8000, interrupt 0 = SPI 133 through ArmCpu)
PianoUsbHost: stage 32: AppleUSBXHCI through the codeless personality (registerService)
PianoUsbHost: nub: interrupt 0 registered by AppleUSBXHCI: SPI 133 (INTID 165, level) attached, readback 0xa003 router 0x…; masked until enabled
PianoUsbHost: attach: a client on the nub
PianoUsbHost: attach: client AppleUSBXHCI, power state N
PianoUsbHost: stage 33: watch of Apple's xHCI (registers from our side, interrupt counts, IOUSBHostDevice services)
PianoUsbHost: Apple xHCI +0 s: USBCMD 0000000… USBSTS 0000000… PORTSC … … DCBAAP 0x… ERSTBA 0x… ERDP 0x… (below 4 GiB | 64-bit addresses: the write order matters); power state N; USB devices 0 (last 0000:0000); interrupt SPI 133: N entries 0 strays 0 polls, N enables N disables
PianoUsbHost: host summary: switch ok | probe PASS | nub up | AppleUSBXHCI attached | USB devices 0 (last 0000:0000) | interrupt SPI 133 | host kept
```

What each outcome means (the order of the questions in §10): no `kext load 0` / `class NOT registered` → the image was not built
with `PIANO_USBHOST_STACK=1`. `no client within 10 s` → H6b: `allocClassWithName … gave nothing` (abstract) or `start FALSE` (the
base class wants something only a subclass gives) are **A2's trigger** (the analysis was approved 2026-11-14 15:33, §5.3). A client with `USBCMD` RS 1, a
growing `entries`, and HCH set again after a while with the power state falling → the idle-power risk (QcomDwc3's subclass keeps the
controller on: also A2). DCBAAP/ERSTBA above 4 GiB with `HSE` or no events → the 64-bit write order (Apple's base class writing the
low half first): our own 32-bit IOMapper on stream 0x40 (no Apple ABI) or A2's 64-bit write override. A device on the port (with
VBUS, as H5c) counted under `USB devices` with its idVendor:idProduct is the H6 pass's first half (ioreg: IOUSBHostDevice).

### 8.3 H3/H4 test images and the batched tablet list (`piano-usbhost=3|4`)

Built like image G2 (charging.md §5e), in the main checkout (`PIANO_K27_TREE` from `~/.config/piano/env`): RAM boots through
`tooling/native/native-loop.sh IMAGE out/ram-dump/boot-piano-macos-ram-A-bundled.img`; the logs from the pstore mailbox
(`logs/native-runs/<ts>-loop/xnu-console.txt`) and, while the Mac cable is in, the USB console (`python3 tooling/native/usb-console.py
--capture 1000 &`). Every image runs PianoPower at level 2 **with writes** (the board configuration is the H3 prerequisite; it also
writes the input limit and charge current, as image I / P1 do). Common prefix:

```sh
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_MENU_TIMEOUT=0 PIANO_RESET_AFTER=420 \
  PIANO_EXTRA_ARGS="exec-log=1 piano-usb=2 piano-power=2 <the image's arguments>" tooling/k27/build-native-image.sh
mkdir -p out/ram-usbc/<image> && cp out/ram/boot-piano-macos-ram-A-bundled.img out/ram/build-A-bundled.json out/ram-usbc/<image>/
grep -E 'PianoUsbHost: (level|port|ucsi|role|hand-off|stage 4|host|No-Op|port [0-9])|PianoPower: (ucsi|mca: (USB-C|board|write)|\[[0-9]+s\] (usb|diag port))|PianoDwc3: hand-off' \
  logs/native-runs/<ts>-loop/xnu-console.txt
```

| # | Image | Arguments | Attended? | Plugs | What it answers |
|---|---|---|---|---|---|
| U0 | `out/ram-usbc/U0` | `piano-usbhost=3` | **unattended** (the Mac cable stays in) | the Mac's USB-C port | the relay under PianoPower (ready after the quiet), the port notification's payload for the Mac (orientation, mux), gpio61 against it; no UCSI command at all (the lowest-risk image) |
| U1 | `out/ram-usbc/U1` | `piano-usbhost=4` | **unattended** (the Mac cable stays in) | the Mac | the UCSI init on the real PPM (does it notify completions: `notified` vs `polled`), the Mac as **DFP**, we **sink**, its power mode (the 3.0 A Rp: `Type-C 3.0 A`, or PD), role **device**, the console unaffected; idle afterwards (no line until a plug) |
| A1 | `out/ram-usbc/A1` | `piano-usbhost=4` (switch off) | attended | Mac → out (+60 s) → OTG adapter + USB stick (+75 s, 45 s) → out → OTG adapter + keyboard (+30 s) → out → Mac (by +300 s); then the same with the plug flipped | **H3**: VBUS from the DSP for the stick (its LED; `we are the provider`; PianoPower's `usb:` and `diag port:` lines; the boost values); **H4**: partner **UFP** → role **host** (`would switch`), unplug → device, the Mac again → DFP; the three orientation witnesses in both orientations |
| A2 | `out/ram-usbc/A2` | `piano-usb-handoff=1 piano-usbhost=4 piano-usbhost-handoff=5 piano-usbhost-reset=1` | attended | as A1 (stick, keyboard, the Mac last) | the live switch: PianoDwc3 released, our host switch and probe, the stick's port connected, reset, **enabled, high speed**; the unplug: back to device, PianoDwc3's takeover again; the Mac's console enumerates afterwards and its replay shows the session |
| A3 | `out/ram-usbc/A3` (`PIANO_USBHOST_STACK=1`) | as A2 with `piano-usbhost-handoff=6` | attended, after A2 passes | stick | Apple's xHCI on the nub inside a live session: `USB devices 1`, the stick's idVendor:idProduct, a mount (H6's question, §8.2) |

Expected lines, U1 (values to be measured; `…` elides the survey of §8):

```
PianoUsbHost: start: piano-usbhost=4 delay=0 xhci=1 qmp=1 spmi=0 watch=300 period=1000 (DWC3 0xa600000, QMP 0x88e8000, orientation gpio61)
PianoUsbHost: level 4: the UCSI client (PPM_RESET, notifications, GET_CAPABILITY, GET_CONNECTOR_STATUS) and the port notifications from PianoPower (its USB-C relay and port events; needs piano-power=2); the role, power direction and orientation published; role switch off (debounce 1000 ms)
…
PianoUsbHost: stage 40: port manager: the UCSI client over PianoPower's USB-C relay, the port notifications from its port events
PianoUsbHost: port: attached to PianoPower's USB-C relay (the UCSI owner 0x800b) and its port events (slot taken, 0 seen before); the role switch is off (piano-usbhost-handoff=0: the decision is published, nothing is switched)
PianoUsbHost: stage 41: port manager: running (event-driven)
PianoUsbHost: role: device: no Type-C state: device mode (the debug cable's default)
PianoUsbHost: port: ucsi waiting | port ? | orientation - | vbus - | role device | hand-off off
PianoPower: mca: msm/adsp/charger_pd is up: reading the charger properties
PianoPower: ucsi: reading the buffer of owner 0x800b (read only)
PianoPower: ucsi: owner 0x800b answers: version 1.10, CCI 0, 48-byte buffer (UCSI 1.x), return code 0
PianoPower: mca: USB-C port notifications enabled: owner 0x800c answered the enable (opcode 0x15, cmd 0x10)
PianoUsbHost: port: notification: port 0, orientation 0 (normal), mux 1 (USB3 only), SVID 0x0000, 32 bytes        (if the DSP reports the state after the enable)
PianoPower: mca: board configuration taken (CP state, OTG configuration, CID): the policy follows
PianoPower: quiet: released: the charger is configured
PianoUsbHost: port: the DSP's USB-C owners are ready (generation 1): the UCSI client starts (PPM_RESET first)
PianoUsbHost: ucsi: connector 1 status XXXXXXXX XXXXXXXX XX: connected, partner DFP (flags 0x1), we are the consumer (we sink), power mode Type-C 3.0 A, changes 0x…, RDO 0x…, battery charging N (the init's)
PianoUsbHost: ucsi: ready: UCSI 1.10, 48-byte mailbox, 1 connector(s) (attributes 0x…, features 0x…, N alternate modes, BC …, PD …, Type-C …); connector 1 capability 0x… (DRP, USB3, alt modes, provider, swaps to DFP); notifications 0x…; 5 commands, completions 5 notified 0 polled, 0 busy
PianoUsbHost: role: device: UCSI: the partner is a DFP (a computer)
PianoUsbHost: port: ucsi ready | port DFP | orientation normal | vbus sink | role device | hand-off off
```

Then nothing more from PianoUsbHost until a plug (the thread sleeps without a timer). `ioreg -rc PianoUsbHost -l | grep piano-usbhost`
over the console's shell shows the same in the properties. U0 has no `ucsi:` lines; its summary reads `ucsi off | port connected
| orientation …` from the notification and gpio61, role device.

A1, the stick (H3 + H4, nothing switched):

```
PianoUsbHost: ucsi: connector 1 status …: nothing attached, partner none (flags 0), -, power mode none, changes 0x4000, …      (the Mac out)
PianoUsbHost: role: device: UCSI: nothing connected (device mode stays armed for the debug cable)
PianoUsbHost: port: notification: port 0, orientation 2 (none), mux 0 (no connection), SVID 0x0000, 32 bytes
PianoUsbHost: ucsi: connector 1 status …: connected, partner UFP (flags 0x1), we are the provider (we source VBUS), power mode USB default, changes 0x5000, …
PianoUsbHost: role: host: UCSI: the partner is a UFP (a peripheral, a hub or a dock's data side) (would switch to host: piano-usbhost-handoff=0)
PianoUsbHost: port: notification: port 0, orientation 1 (reverse), mux 1 (USB3 only), SVID 0x0000, 32 bytes
PianoUsbHost: port: ucsi ready | port UFP | orientation reverse | vbus source | role host | hand-off off
PianoPower: [Ns] usb: a device is attached (we source) on CC2, …
PianoPower: [Ns] diag port: verify (0x21002) 0, PD state (0x2100d) …, data role (0x2100c) 2?, OTG boost (0x200d) NONZERO, gate (0x200e) NONZERO, plug-in (0x21020) 1?, Type-C mode (0x21015) …, CC toggle …, sink/source (0x21022) …, online …
```

Pass H3: the stick's LED, `we are the provider (we source VBUS)`, PianoPower's `a device is attached (we source)`, the boost values
non-zero (note them: they are the encodings §2.4 lacks); no charging regression once the Mac is back (the gauges' current as before).
Pass H4: UFP / host for the stick and the keyboard, DFP / device for the Mac, nothing / device in between; the witnesses agree in both
orientations (`orientation reverse` with the plug flipped, no `DISAGREE`). A dock, if one is at hand: DFP first, then a partner
change to UFP (`changes 0x0800`) and role host after its swap, `we are the consumer` (it powers us).

A2, the switch (after the UFP status, 1 s debounce):

```
PianoUsbHost: hand-off: PianoDwc3 asked to release the controller: released
PianoDwc3: hand-off: released to PianoUsbHost (1): DCTL … DSTS … (halted); the console keeps its log for the replay
PianoUsbHost: hand-off: host session 1: the switch, then our xHCI probe
PianoUsbHost: stage 10: host switch: …                                   (§8.1's H5a lines: the switch, HCRST, rings, run)
PianoUsbHost: No-Op: event …: type 33 code 1 for TRB 0x… (expected 0x…) after N us, …: PASS
PianoUsbHost: port 1 (USB 2) PORTSC 00000203…: connected 1 enabled 0 power 1 …
PianoUsbHost: port 1: reset: PORTSC … -> … in N ms: enabled, high speed
PianoUsbHost: hand-off: host ports 1:high 2:-
PianoUsbHost: hand-off: host mode
…  (the unplug)
PianoUsbHost: ucsi: connector 1 status …: nothing attached …
PianoUsbHost: role: device: UCSI: nothing connected (device mode stays armed for the debug cable)
PianoUsbHost: hand-off: host session 1 ends after N s (M port changes): back to device mode
PianoUsbHost: back: GCTL …2… (PRTCAPDIR 2) …; override restored 1; N stores: device role (PianoDwc3's takeover programs the rest)
PianoUsbHost: hand-off: PianoDwc3 reclaims the controller: device (PianoDwc3 runs the controller)
PianoDwc3: hand-off: reclaim 1: the core is back from PianoUsbHost: the takeover again
PianoDwc3: stage 10: takeover: power and clocks … stage 19: takeover: run
PianoUsbHost: hand-off: device mode
```

Pass: the same for the keyboard (`enabled, low|full speed`), the Mac's console enumerates when it is plugged back and its replay holds
every line above. Reading the failures: `PianoDwc3 keeps the controller` → the image lacks `piano-usb-handoff=1`;
`released` never comes (`device`) → PianoDwc3's thread did not take the ask within 2 s (its log); a stage 10–17 failure → §8.1's
reading; `reclaim … clocks read off` → the core lost its clocks in the session (no takeover was tried, by design: reboot).

Risks of the batch (also §6.6): U1/A1/A2 send UCSI commands to the DSP's PPM for the first time (PPM_RESET included): a firmware that
handles them badly could stall its Type-C state machine (charging included) until the next DSP start, which natively is the next boot;
U0 first isolates that. A2 writes the controller (as H5) and hands it between two kexts for the first time.

### 8.4 The first A2 probe (`out/ram-a2`, 27.3, unattended, path A2)

The image and its dump companion are built by `private/host-checks/20261114-a2/build-image.sh` (local; no device access) from the
A2 collection of `PIANO_USBHOST_A2=1 tooling/k273/build-k273-image.sh`. Arguments: the installed `macos-27.3` line of
`out/persistent/esp-k273-v2.json` plus its `boot_args_split` tail, without `piano-boot-args=macos`, `data-store-lba` and
`data-persist=2`; `data-persist=0` (the owner's Data store is never opened: seed `/Volumes/ExtSSD/k273/v2/seed-200.img`, trust
`…/layer/kernel.macho.trustcache`); PianoDwc3's `piano-usb*` arguments removed, because at level 6 the host probe owns the
controller (`piano-usb=0`, so **there is no USB console and no root shell in this boot** — the log is the pstore mailbox, pulled by
`out/ram-a2-dump`); then

```
piano-usb=0 piano-usbhost=6 piano-usbhost-a2=1 piano-usbhost-delay=30 piano-usbhost-attach=3 piano-usbhost-irq=1
piano-usbhost-probe=1 piano-usbhost-smmu=2 piano-usbhost-watch=600 piano-usbhost-period=1000 piano-usbhost-back=0
piano-reset-after=900 piano-reset-to=bootloader
```

Built 2026-11-14 from the A2 collection `660bced2…` (the ADT, trust cache and seed of the installed layout): the probe is
**238,329,856 bytes**, sha256 `18e0ca97916e97d6264b89e65830ccbb226659f3d6a21c069e5226d91f457117`, its dump companion
`a0e0a6e92acd44f4362830aac42aa3894421b0127be95b00a953f15f3ed643fa`; both `verify-ram-images.py PASS: 0 failure(s)` and below the
250,000,000-byte limit. Run it like the other RAM probes: `tooling/native/native-loop.sh
out/ram-a2/boot-piano-macos-ram-A-bundled.img out/ram-a2-dump/boot-piano-macos-ram-A-bundled.img`, then
`grep -E 'PianoXhciHost|PianoUsbHost' logs/native-runs/<ts>-loop/xnu-console.txt`. Nothing needs a hand on the tablet; with
nothing in the port the probe still answers questions 1–9 below. **`piano-touch=2` is in the line but PianoTouch is not in this
collection** (the stack's kexts keep their addresses and leave no `__DATA_CONST` gap for it, §5.2a): expect no `PianoTouch:` line
and do not read that as a touch regression. End the run as soon as the lines below are read (`reset fastboot`): there is no reason
to wait out the 900 s.

**What this boot must answer, and the line that answers it** (values to be measured; the log is the mailbox copy):

| # | Question | The line |
|---:|---|---|
| 1 | Is the A2 subclass in the collection, and is its instance size the original's? | `PianoXhciHost: linked (AppleUSBXHCI subclass, 3 overrides: …; instance size 3976 bytes, the original's)` at boot. 3976 = `0xf88`. No line → the image was not built with `PIANO_USBHOST_A2=1` |
| 2 | Does the kernel start the A2 kext at all (the collection links AppleUSBXHCI and IOUSBHostFamily at boot for it)? | the same line, before any `PianoUsbHost:` stage; a panic in `OSKext::load` instead would name the kext |
| 3 | Does level 6 pick our class? | `PianoUsbHost: level 6: attach=3 irq=1 probe=1 a2=1 (our own subclass: slot 292 power on, slot 419 read64 low-high, slot 421 write64 high-low)` and `PianoXhciHost: kext load 0x0, class registered (instance size 3976) (path A2: …)` |
| 4 | Does the hardware path still pass before Apple's code runs (the H5 gate: the switch, HCRST, the rings, the No-Op)? | `PianoUsbHost: No-Op: event …: type 33 code 1 for TRB 0x… …: PASS`, then `xHCI stop before our subclass of Apple's driver: halted and reset: … ok`. A failure here is an H5 regression, not an A2 result: read §8.1's failure table |
| 5 | Does the identity-mapped SMMU bank still come up? | `PianoUsbHost: smmu: stream 0x40: identity map of DRAM from 2 GiB, write-back inner shareable (coherent) …` |
| 6 | Does IOKit matching find our personality on the nub? | `PianoUsbHost: attach: a client on the nub` then `attach: client PianoXhciHost, power state N`. If not: `no client within 10 s …: allocating the class by name now (piano-usbhost-attach=3)` followed by `attach: PianoXhciHost allocated (3976 bytes), init 1, start true|FALSE` — the same boot then also answers whether `start()` accepts this nub |
| 7 | Does Apple's `start()` accept our nub (A1's first open risk)? | `start true` (attach=3's line), or a client appearing at all; `start FALSE` names the base class's refusal as the next thing to read |
| 8 | **Does the 64-bit write override get used, and in the right order?** (the A2 reason) | `PianoXhciHost: write64 #1 base 0x… +0x30 = 0x… (high 0x… first, then low 0x…)` … up to #8, and `read64 #…` lines. The offsets should be the ring registers (DCBAAP `+0x30`, CRCR `+0x38`, ERSTBA `+0x30` of the interrupter, ERDP `+0x38`); **no `write64` line at all** means Apple's driver never used the 64-bit accessor (then the DCBAAP/ERSTBA values of question 9 say whether it wrote the halves itself). The `base` value is the second thing to read here: that the accessor's base argument is the mapped register window is **inferred** from QcomDwc3; an implausible value, or a fault at the first 64-bit access, means it is something else |
| 9 | Are Apple's structures where we can see them, and does the controller run? | `PianoUsbHost: Apple xHCI +0 s: USBCMD 0000000… USBSTS … PORTSC … DCBAAP 0x… ERSTBA 0x… ERDP 0x… (below 4 GiB \| 64-bit addresses: the write order matters); power state N; USB devices 0 …; interrupt SPI 133: N entries 0 strays 0 polls …` — read USBCMD bit 0 (RS) and USBSTS bit 0 (HCH): running and not halted is the pass. Rings above 4 GiB with correct high halves are the strongest evidence the override works |
| 10 | Does the power override hold (A1's third risk)? | `power state 3` in every `Apple xHCI +…` line for the whole watch, and `USBCMD` keeping RS 1 (A1 was expected to idle to off) |
| 11 | Does the interrupt line work or does it have to be polled? | the `interrupt SPI 133: N entries …` counters growing in later lines; `0 entries` with events arriving means the poll fallback is doing the work (`piano-usbhost-irq=0` isolates it) |
| 12 | Did anything of Apple's stack load that should not have? | `PianoUsbHost: stack: AppleUSBHostResources up (IOUSBHostFamily loaded through the gate)` is expected; no `IOUSBMassStorageDriver`/hub/HID matching lines before a device is attached |
| 13 | The summary, for the result table | `PianoUsbHost: host summary: switch ok \| probe PASS \| nub up \| PianoXhciHost attached \| USB devices 0 (last 0000:0000) \| interrupt SPI 133 \| host kept` |
| 14 | Did the boot stay healthy (the rest of the image is the installed profile)? | `piano-boot-guard: healthy after 180 s`, `NativeRamBlock: Data store state` 1/3 (volatile, **never** 2), the Wi-Fi and charging lines as in the installed boot, and the return to fastboot at 900 s |

With a device in the port (attended, later): `USB devices 1` with its `idVendor:idProduct` and, for a stick, a mount — that is the
H6 pass. This first boot deliberately needs nobody at the tablet: a bus-powered device would also need the DSP's VBUS (H3).

**Failure readings specific to A2.** A panic at the first virtual call into the subclass (before any `attach:` line) would mean a
PAC failure, i.e. the ABI data no longer matches the collection — but the build gate refuses that combination, so the first thing
to check is that the image's collection is the one the gate ran against (`out/k273-a2/kc-k27-native.bin`). `class NOT registered`
with the A2 line present means the metaclass did not register: read `OSKext` lines for AppleUSBXHCI. A `start FALSE` with no
`write64` line means Apple's start refused before touching a 64-bit register (a provider property or a resource it wants, not the
accessors).

## 9. Fallback

Without `piano-usbhost` nothing changes: PianoDwc3 owns the port in device mode (console, HID, NCM), PianoPower reads the Type-C
state for charging (and its relay has no client: PianoMca sees every message as before); without `PIANO_USBHOST_STACK=1` the collection is the usual one, and with it nothing of Apple's stack loads
unless `piano-usbhost=6` opens the gate. At levels 5 and 6 a failed step stops the session where it failed (logged with its stage);
with `piano-usbhost-back=S` the core is returned to device mode, else it stays as left until the next boot (the firmware resets the
controller for its own device mode). Later, the role manager (H8) returns the core to device mode itself.

## 10. Open questions

1. PRTCAPDIR, the IP revision (soft reset needed?), the root ports and their protocols after the UEFI → H1.
2. Does the PMIC's CC bit agree with gpio61 (high = reverse) and the PHY's select in both orientations? → H1b.
3. Who owns the repeater's and the Type-C block's APIDs for writes (EE 0?) → H1 `spmi:` line.
4. Does the DSP switch the port to dual role and source VBUS by itself after the board configuration, and what do 0x21015 /
   0x21022 / 0x200d read? → H3: images A1/A2 (§8.3; the two readings, sourcing and sinking, are the encodings).
5. Is `AppleUSBXHCI` instantiable through a codeless personality, does its base class write 64-bit registers low first, and does
   it take its provider's IOMapper? → H6a/H6b (`attach:` lines; DCBAAP/ERSTBA read from our side; no mapper is offered yet).
6. The `gAppleARMUSBCable*` values IOAccessoryManager and IOUSBHostFamily expect → H6 (`stack: resource AppleUSBCableType`); the
   names are known (link report), the type and values in `UsbCableKeys.cpp` are inferred.
7. One SPI 133 for both roles with no detach/mask in our ArmCpu → **prepared**: the nub attaches it only when PianoDwc3 did not
   (`piano-usb-irq=1`), masks it at the distributor between enables, and polls otherwise; a shared dispatcher (H8) or
   `armcpu_spi_mask` in the kernel (UniXNU's newer ArmCpu) when both roles live in one boot.
8. The OTG re-plug quirk: reproduce at H8 with the DSP's own state (0x21020) logged.
9. Does the identity-mapped stage 1 bank work under Gunyah on piano (only bypass banks are proven natively: NativeUfs, PianoDwc3),
   and does our level 5 probe still pass with the master coherent and our rings non-cacheable (mixed attributes)? → H6a's `smmu:`
   line and its probe; `-smmu=1` and `-probe=0` separate them.
10. The IP revision decides GCTL.CORESOFTRESET (`-csr=2`): H1's GSNPSID/GVERSION; and does the core need SUN's other stock host
    values (GSBUSCFG1, RXTHRCFG, GUCTL1, GUCTL3, DSBLCLKGTNG) for transfers (not for a No-Op)? → H5c/H6 with a device.
11. Does host mode need the eUSB2 repeater's 19.2 MHz clock force (Linux's host-mode workaround) for connection or only for L2? → H5c.
12. A largest-first placement in the KC builder (k27 tree) would let PianoTouch stay in the stack build (§5.2a).
13. Does the DSP's PPM notify command completions (or only connector changes), and does it take PPM_RESET from us with charging
    unaffected? → U1's `ucsi: ready … completions N notified M polled` and the gauges' current before/after (§8.3); U0 first.
14. Does GET_CONNECTOR_STATUS give UFP for an OTG adapter with a stick, a keyboard, and for a dock after its swap (and DFP for the
    Mac), with the power direction right? → A1.
15. The port notification's payload on piano (32 bytes? orientation with nothing attached? the mux value for a USB 2 device: "USB3
    only" or "none"?), and does its orientation agree with gpio61 in both plug orientations? → U0/A1 `port: notification` lines.
16. Does PianoDwc3's takeover run cleanly a second time after a host session (event buffer, FIFOs, the UTMI selection kept), and
    does the Mac's CDC-ACM driver enumerate it again? → A2.
17. PianoPower's built-in policy counts our own 5 V as a source attached while we source (§6.4): its owner's decision.

## 11. Licence note

PianoUsbHost, PianoUsbHostStandIn and PianoXhciHost are BSD-2-Clause-Patent (`LICENSE`, `UPSTREAM` in each package). Linux
(GPL-2.0) is the specification only: behaviour and offsets cited by file and function, nothing pasted (dwc3 and xhci for level 5,
the `write-64-hi-lo-quirk` fact for A2). QcomDwc3 and Project-SunUEFI are BSD-2-Clause-Patent: `SmmuIdentity.h` adapts QcomDwc3's
`smmuIdentity()` with attribution, the nub follows QcomDwc3's nub as facts and shape; QcomDwc3's own `AppleUSBXHCI` subclass was
read as the **design decision** behind A2 (which three slots, and why each one) and is cited in `package/PianoXhciHost/UPSTREAM`,
with the code written anew here; their GPL PHY headers are not used.

**Path A2 and Apple's binary** (owner's approval, 2026-11-14): the class's vtable layout was analysed offline
(`private/host-checks/20261114-xhci/REPORT.md`, local only) and the result reused from UniXNU-sheng's reviewed fixture of the same
collection. What is in this repository is **our analysis data**: slot numbers, PAC diversities, symbol names, the instance size and
synthetic declaration names (`data/usb-xhci/abi-26C5035r-mac17j.json`, provenance in `data/usb-xhci/README.md`). No Apple code,
Apple bytes, Apple binary or payload is committed, and the generated declaration header and thunks are build outputs under
`out/native-kexts/PianoXhciHost/`. From the collection itself only kext names, class names, OSBundleLibraries, Info.plist matching
dictionaries, the link report's symbol names (§5) and that ABI data are used.
The UCSI client (`Ucsi.h`, `UcsiClient.h`) is written from the UCSI specification's layouts and command set; Linux's ucsi.c,
ucsi_glink.c and pmic_glink_altmode.c (GPL-2.0) were read as described behaviour (the init order, the reset's polling, the
acknowledgements, the supported-notifications rule, the GLINK framing and the port notification's layout), nothing pasted; QcomDwc3's
`QcomUcsi.h` (BSD-2-Clause-Patent) gave the framing facts. PianoPower's relay and PianoDwc3's hand-off are this repository's own.
