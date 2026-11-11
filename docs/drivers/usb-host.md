# USB host / OTG and the Type-C port: PianoUsbHost

> **Status 2026-11-11: offline, built, host-tested, linked; nothing run on hardware.** Backlog item B7 (`../BACKLOG-hardware.md`).
> `package/PianoUsbHost`, inert without `piano-usbhost`: level 1/2 a read-only survey; **level 5 (H5)** host mode with our own
> minimal xHCI probe (host switch, HCRST, rings, a No-Op command, port connection, optional port reset, way back), host-tested
> against a register model (`XhciHost.h`, §6.2a); **level 6 (H6 preparation, path A1)** Apple's `AppleUSBXHCI` on our
> `PianoUsbHostNub` through a codeless personality, stream 0x40 identity-mapped write-back (`SmmuIdentity.h`, after QcomDwc3),
> interrupt 0 over SPI 133 or polled (§6.5a). Apple's host stack links natively with `PIANO_USBHOST_STACK=1` (§5.2a: a new
> stand-in, the cable keys in ARMPlatformStandIn for that build only, the stack's boot-time personalities gated; PianoTouch is left
> out of that build for want of a `__DATA_CONST` gap). The default collection is unchanged but for PianoUsbHost's own objects.
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

### 6.4 VBUS

Host for a bus-powered device needs: the DSP up (PianoPower level 2), the port in dual-role mode (the DSP's, 0x21015 to read),
the OTG board configuration (opt-in write `0x201c`, `0x201a`, `0x201b`; PianoPower's allowlist change), then the DSP sources VBUS
by itself (boost status 0x200d, notification 3/10). No native code switches a boost. A powered dock needs none of this.

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

## 7. Boot args and component

| Boot arg | Default | Effect |
|---|---|---|
| `piano-usbhost=N` | 0 (probe declines, nothing mapped) | 1 read-only survey; 2 survey + watch of the port; **5** the survey, then host mode with our own xHCI probe (§6.2a); **6** the survey, the switch, then Apple's xHCI on our nub (§6.5a); 3, 4 and others run as 2 |
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
| `piano-usbhost-attach=0/1/2` | 1 | level 6: the nub only, the codeless personality, an allocation by name |
| `piano-usbhost-irq=0/1` | 1 | level 6: polled every 1 ms, SPI 133 (polled anyway when the INTID is taken) |
| `piano-usbhost-probe=0/1` | 1 | level 6: our No-Op probe before Apple's driver |

Levels 5 and 6 refuse (and run the level 1 survey) while PianoDwc3 owns the controller (`piano-usb>=2`). The one exception is level
5's **hand-off by time**: `piano-usbhost-back=S` and `piano-usb-delay >= piano-usbhost-delay + S + 30`: the host session ends, the core
is back in device mode, and PianoDwc3's takeover (its own halt, soft reset and device programming) starts later; its replay of the
retained log then shows the host session on the Mac. No PianoDwc3 code is involved (H8 adds a real hand-off).

Component: `PianoUsbHost` in `configs/k27-native-kexts.txt` (`tooling/k27/kext/PianoUsbHost.json`, bundle
`com.unixnu.driver.PianoUsbHost`, IOService on `IOPlatformExpertDevice`, kpi libraries only); build
`tooling/k27/build-native-piano-usbhost.sh` (three host tests). Level 6 also needs the collection from `PIANO_USBHOST_STACK=1`
(`package/PianoUsbHostStandIn`, `tooling/k27/kext/PianoUsbHostStack.json`, §5.2a); on the default collection it logs the class as
not registered and attaches nothing.

## 8. Hardware test plan and ladder

RAM boots, owner present, Ubuntu returns on reboot. Each step is opt-in by boot argument; H1 writes nothing.

| Step | What | Pass | Effort |
|---|---|---|---|
| H1 survey | `piano-usbhost=1` (+ `piano-usb=2` for the console, or alone) with the Mac cable | the lines below, no hang; facts: PRTCAPDIR, IP version, xHCI caps and ports if clocked, PHY mode/orientation, port mode, repeater owner | built; 1 session |
| H1b watch | `piano-usbhost=2`: plug Mac, charger, OTG adapter with a keyboard, both orientations; again with `piano-power=2 piano-usbhost-spmi=1` | changes logged; DSP off: no attach for the OTG adapter (sink-only); DSP on: source mode + Rd; witnesses agree | 1 session |
| H2 DSP view | PianoPower: read 0x21015/0x21022/0x200d/0x200e/0x21020, decode the PAN payload, UCSI READ_BUF (VERSION, CCI) only | values for each plug case | 1–2 d + 1 session |
| H3 OTG power | opt-in board configuration writes | VBUS on an OTG adapter (keyboard LED), boost status non-zero, no charging regression | 1 d + 1 session |
| H4 UCSI | PPM_RESET … GET_CONNECTOR_STATUS, notifications (from QcomUcsi.h) | partner UFP for the adapter, DFP for the Mac, dock UFP after its swap | 2–3 d + 1 session |
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
base class wants something only a subclass gives) are **A2's trigger** (the owner's decision, §5.3). A client with `USBCMD` RS 1, a
growing `entries`, and HCH set again after a while with the power state falling → the idle-power risk (QcomDwc3's subclass keeps the
controller on: also A2). DCBAAP/ERSTBA above 4 GiB with `HSE` or no events → the 64-bit write order (Apple's base class writing the
low half first): our own 32-bit IOMapper on stream 0x40 (no Apple ABI) or A2's 64-bit write override. A device on the port (with
VBUS, as H5c) counted under `USB devices` with its idVendor:idProduct is the H6 pass's first half (ioreg: IOUSBHostDevice).

## 9. Fallback

Without `piano-usbhost` nothing changes: PianoDwc3 owns the port in device mode (console, HID, NCM), PianoPower reads the Type-C
state for charging; without `PIANO_USBHOST_STACK=1` the collection is the usual one, and with it nothing of Apple's stack loads
unless `piano-usbhost=6` opens the gate. At levels 5 and 6 a failed step stops the session where it failed (logged with its stage);
with `piano-usbhost-back=S` the core is returned to device mode, else it stays as left until the next boot (the firmware resets the
controller for its own device mode). Later, the role manager (H8) returns the core to device mode itself.

## 10. Open questions

1. PRTCAPDIR, the IP revision (soft reset needed?), the root ports and their protocols after the UEFI → H1.
2. Does the PMIC's CC bit agree with gpio61 (high = reverse) and the PHY's select in both orientations? → H1b.
3. Who owns the repeater's and the Type-C block's APIDs for writes (EE 0?) → H1 `spmi:` line.
4. Does the DSP switch the port to dual role and source VBUS by itself after the board configuration, and what do 0x21015 /
   0x21022 / 0x200d read? → H2/H3.
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

## 11. Licence note

PianoUsbHost and PianoUsbHostStandIn are BSD-2-Clause-Patent (`LICENSE`, `UPSTREAM` in each package). Linux (GPL-2.0) is the
specification only: behaviour and offsets cited by file and function, nothing pasted (dwc3 and xhci for level 5). QcomDwc3 and
Project-SunUEFI are BSD-2-Clause-Patent: `SmmuIdentity.h` adapts QcomDwc3's `smmuIdentity()` with attribution, the nub follows
QcomDwc3's nub as facts and shape; otherwise only facts and design are taken (their GPL PHY headers are not used, nor QcomDwc3's
AppleUSBXHCI subclass: path A2). From Apple's kernel collection only kext names, class names, OSBundleLibraries, Info.plist matching
dictionaries and the link report's symbol names are used (§5); no Apple binary was disassembled, and no vtable fixture exists.
